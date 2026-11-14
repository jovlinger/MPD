/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Walk.hxx"
#include "UpdateIO.hxx"
#include "Editor.hxx"
#include "UpdateDomain.hxx"
#include "db/DatabaseLock.hxx"
#include "db/Uri.hxx"
#include "db/plugins/simple/Directory.hxx"
#include "db/plugins/simple/Song.hxx"
#include "storage/StorageInterface.hxx"
#include "ExcludeList.hxx"
#include "fs/AllocatedPath.hxx"
#include "fs/Traits.hxx"
#include "fs/FileSystem.hxx"
#include "storage/FileInfo.hxx"
#include "input/InputStream.hxx"
#include "input/Error.hxx"
#include "util/StringAPI.hxx"
#include "util/StringCompare.hxx"
#include "util/UriExtract.hxx"
#include "Log.hxx"
#include "fs/Path.hxx"

#include <cassert>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifndef _WIN32
#include <unistd.h>
#endif

/**
 * Observability for the efficient walk (todo:d6d78baa).
 * Tunable: set MPD_WALK_METRICS=0 to disable file output.
 */
void
WalkMetricsEmit(const char *kind, const UpdateWalk &walk,
		const char *extra) noexcept
{
	const char *env = getenv("MPD_WALK_METRICS");
	if (env != nullptr && env[0] == '0')
		return;

	FILE *f = fopen("/tmp/mpd-walk-metrics.jsonl", "a");
	if (f == nullptr)
		return;

	const auto now = std::chrono::system_clock::now().time_since_epoch();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
	fprintf(f,
		"{\"ts_ms\":%lld,\"kind\":\"%s\",\"dirs_entered\":%llu,"
		"\"dirs_skipped_done\":%llu,\"alias_skipped\":%llu,"
		"\"residual_index_skipped\":%llu,"
		"\"sym_skipped_done\":%llu,\"songs\":%llu,"
		"\"done_ids\":%zu,\"done_paths\":%zu,"
		"\"max_dir_entries\":%llu%s%s}\n",
		(long long)ms,
		kind,
		(unsigned long long)walk.metric_dirs_entered,
		(unsigned long long)walk.metric_dirs_skipped_done,
		(unsigned long long)walk.metric_alias_skipped,
		(unsigned long long)walk.metric_residual_index_skipped,
		(unsigned long long)walk.metric_symlinks_skipped_done,
		(unsigned long long)walk.metric_songs,
		walk.done_ids.size(),
		walk.done_real_paths.size(),
		(unsigned long long)walk.metric_max_dir_entries,
		extra != nullptr && extra[0] != '\0' ? "," : "",
		extra != nullptr ? extra : "");
	fclose(f);
}

/**
 * Browse mirrors live only at files/_tags and files/_meta (or music-root
 * _tags/_meta).  Residual album-local _tags (and any other off-mirror copy)
 * link back into the global tree; on CIFS those links are reified dirs and
 * must not be descended or the walk explodes before real albums finish.
 */
gcc_pure
static bool
IsIndexTreeName(const char *name) noexcept
{
	return StringIsEqual(name, "_tags") || StringIsEqual(name, "_meta");
}

/**
 * A child literally named _tags/_meta is a browse-root only as a direct
 * child of music root or of files/.  Anywhere else (album-local mirrors,
 * or albums reached via files/_tags/.../album) it is residual adjacency
 * into the global tag tree -- emit directory: but do not descend.
 */
gcc_pure
static bool
ParentAllowsIndexTreeDescend(const char *parent_path) noexcept
{
	if (parent_path == nullptr || parent_path[0] == '\0')
		return true;
	return StringIsEqual(parent_path, "files");
}

/**
 * files/_meta/<Album> may still carry residual artist/genre/year links into
 * files/_tags (shadup history).  Only the album -> real-album child is the
 * one-way contract; descending the others on CIFS re-enters the tag tree
 * under a _meta URI and can ENAMETOOLONG / drop files/_tags.
 */
gcc_pure
static bool
IsMetaAlbumDir(const char *parent_path) noexcept
{
	constexpr char prefix[] = "files/_meta/";
	if (parent_path == nullptr ||
	    !StringStartsWith(parent_path, prefix))
		return false;
	const char *rest = parent_path + sizeof(prefix) - 1;
	return rest[0] != '\0' && std::strchr(rest, '/') == nullptr;
}

gcc_pure
static int
NameClass(const char *name) noexcept
{
	if (name == nullptr || name[0] == '\0')
		return 2;
	const unsigned char ch = (unsigned char)name[0];
	if (ch >= '0' && ch <= '9')
		return 0;
	if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
		return 1;
	return 2;
}

#ifndef _WIN32
/**
 * Resolve storage-relative UTF-8 directory URI to a real absolute path
 * for the done-set.  Empty string on failure.
 */
static std::string
StorageRealPath(Storage &storage, const char *uri_utf8) noexcept
{
	const auto path_fs = storage.MapFS(uri_utf8);
	if (path_fs.IsNull())
		return {};

	char buf[PATH_MAX];
	if (realpath(path_fs.c_str(), buf) == nullptr)
		return path_fs.c_str();
	return buf;
}
#endif

gcc_pure
bool
DoneSetContains(const UpdateWalk &walk, const StorageFileInfo &info,
		const char *uri_utf8) noexcept
{
	if (info.inode != 0 || info.device != 0) {
		const WalkDoneId id{info.device, info.inode};
		if (walk.done_ids.find(id) != walk.done_ids.end())
			return true;
	}
#ifndef _WIN32
	if (uri_utf8 != nullptr) {
		const std::string real = StorageRealPath(walk.storage, uri_utf8);
		if (!real.empty() &&
		    walk.done_real_paths.find(real) != walk.done_real_paths.end())
			return true;
	}
#else
	(void)uri_utf8;
#endif
	return false;
}

bool
DoneSetInsert(UpdateWalk &walk, const StorageFileInfo &info,
	      const char *uri_utf8) noexcept
{
	if (info.inode != 0 || info.device != 0) {
		const WalkDoneId id{info.device, info.inode};
		if (!walk.done_ids.insert(id).second)
			return false;
#ifndef _WIN32
		if (uri_utf8 != nullptr) {
			const std::string real =
				StorageRealPath(walk.storage, uri_utf8);
			if (!real.empty())
				walk.done_real_paths.insert(real);
		}
#else
		(void)uri_utf8;
#endif
		return true;
	}
#ifndef _WIN32
	if (uri_utf8 != nullptr) {
		const std::string real = StorageRealPath(walk.storage, uri_utf8);
		if (!real.empty())
			return walk.done_real_paths.insert(real).second;
	}
#else
	(void)uri_utf8;
#endif
	/* No identity available: do not block the walk. */
	return true;
}

UpdateWalk::UpdateWalk(const UpdateConfig &_config,
		       EventLoop &_loop, DatabaseListener &_listener,
		       Storage &_storage) noexcept
	:config(_config), cancel(false),
	 storage(_storage),
	 editor(_loop, _listener)
{
}

static void
directory_set_stat(Directory &dir, const StorageFileInfo &info)
{
	dir.inode = info.inode;
	dir.device = info.device;
}

inline void
UpdateWalk::RemoveExcludedFromDirectory(Directory &directory,
					const ExcludeList &exclude_list) noexcept
{
	const ScopeDatabaseLock protect;

	directory.ForEachChildSafe([&](Directory &child){
		const auto name_fs =
			AllocatedPath::FromUTF8(child.GetName());

		if (IsIndexTreeName(child.GetName()))
			return;

		if (name_fs.IsNull() || exclude_list.Check(name_fs)) {
			editor.DeleteDirectory(&child);
			modified = true;
		}
	});

	directory.ForEachSongSafe([&](Song &song){
		assert(&song.parent == &directory);

		const auto name_fs = AllocatedPath::FromUTF8(song.filename);
		if (name_fs.IsNull() || exclude_list.Check(name_fs)) {
			editor.DeleteSong(directory, &song);
			modified = true;
		}
	});
}

inline void
UpdateWalk::PurgeDeletedFromDirectory(Directory &directory) noexcept
{
	directory.ForEachChildSafe([&](Directory &child){
		if (child.IsMount())
			/* mount points are always preserved */
			return;

		if (DirectoryExists(storage, child) &&
		    child.IsPluginAvailable())
			return;

		/* the directory was deleted (or the plugin which
		   handles this "virtual" directory is unavailable) */

		editor.LockDeleteDirectory(&child);

		modified = true;
	});

	directory.ForEachSongSafe([&](Song &song){
		if (!directory_child_is_regular(storage, directory,
						song.filename) ||
		    !song.IsPluginAvailable()) {
			/* the song file was deleted (or the decoder
			   plugin is unavailable) */

			editor.LockDeleteSong(directory, &song);

			modified = true;
		}
	});

	for (auto i = directory.playlists.begin(),
		     end = directory.playlists.end();
	     i != end;) {
		if (!directory_child_is_regular(storage, directory, i->name)) {
			const ScopeDatabaseLock protect;
			i = directory.playlists.erase(i);
		} else
			++i;
	}
}

#ifndef _WIN32
static bool
update_directory_stat(Storage &storage, Directory &directory) noexcept
{
	StorageFileInfo info;
	if (!GetInfo(storage, directory.GetPath(), info))
		return false;

	directory_set_stat(directory, info);
	return true;
}
#endif

/**
 * Check the ancestors of the given #Directory and see if there's one
 * with the same device/inode number, building a loop.
 *
 * @return 1 if a loop was found, 0 if not, -1 on I/O error
 */
static int
FindAncestorLoop(Storage &storage, Directory *parent,
		 unsigned inode, unsigned device) noexcept
{
#ifndef _WIN32
	if (device == 0 && inode == 0)
		/* can't detect loops if the Storage does not support
		   these numbers */
		return 0;

	while (parent) {
		if (parent->device == 0 && parent->inode == 0 &&
		    !update_directory_stat(storage, *parent))
			return -1;

		if (parent->inode == inode && parent->device == device) {
			LogDebug(update_domain, "recursive directory found");
			return 1;
		}

		parent = parent->parent;
	}
#else
	(void)storage;
	(void)parent;
	(void)inode;
	(void)device;
#endif

	return 0;
}

inline bool
UpdateWalk::UpdateRegularFile(Directory &directory,
			      const char *name,
			      const StorageFileInfo &info) noexcept
{
	const char *suffix = PathTraitsUTF8::GetFilenameSuffix(name);
	if (suffix == nullptr)
		return false;

	return UpdateSongFile(directory, name, suffix, info) ||
		UpdateArchiveFile(directory, name, suffix, info) ||
		UpdatePlaylistFile(directory, name, suffix, info);
}

void
UpdateWalk::UpdateDirectoryChild(Directory &directory,
				 const ExcludeList &exclude_list,
				 const char *name, const StorageFileInfo &info) noexcept
try {
	assert(std::strchr(name, '/') == nullptr);

	if (info.IsRegular()) {
		UpdateRegularFile(directory, name, info);
	} else if (info.IsDirectory()) {
		if (FindAncestorLoop(storage, &directory,
					info.inode, info.device))
			return;

		Directory *subdir;
		{
			const ScopeDatabaseLock protect;
			subdir = directory.MakeChild(name);
		}

		assert(&directory == subdir->parent);

		if (IsIndexTreeName(name))
			FmtWarning(update_domain,
				   "dontwalk-child parent={} name={} allow={}",
				   directory.GetPath(), name,
				   ParentAllowsIndexTreeDescend(directory.GetPath()) ? 1 : 0);

		/* Residual album-local _tags/_meta: keep directory: for
		   lsinfo but do not descend (CIFS reifies those back-edges). */
		if (IsIndexTreeName(name) &&
		    !ParentAllowsIndexTreeDescend(directory.GetPath())) {
			directory_set_stat(*subdir, info);
			DoneSetInsert(*this, info, subdir->GetPath());
			++metric_residual_index_skipped;
			WalkMetricsEmit("residual_index_skip", *this,
					"\"reason\":\"album_local_index\"");
			return;
		}

		/* files/_meta/<Album>/{artist,genre,year}: emit only. */
		if (IsMetaAlbumDir(directory.GetPath()) &&
		    !StringIsEqual(name, "album")) {
			directory_set_stat(*subdir, info);
			DoneSetInsert(*this, info, subdir->GetPath());
			++metric_residual_index_skipped;
			WalkMetricsEmit("residual_index_skip", *this,
					"\"reason\":\"meta_tag_backedge\"");
			return;
		}

		const bool ok = UpdateDirectory(*subdir, exclude_list, info);
		if (IsIndexTreeName(name))
			FmtWarning(update_domain,
				   "dontwalk-child UpdateDirectory({})={}",
				   name, ok ? 1 : 0);
		if (!ok) {
			/* Keep browse-root stubs even if OpenDirectory
			   failed once on a flaky CIFS handle. */
			if (!(IsIndexTreeName(name) &&
			      ParentAllowsIndexTreeDescend(directory.GetPath())))
				editor.LockDeleteDirectory(subdir);
		}
	} else {
		FmtDebug(update_domain,
			 "{} is not a directory, archive or music", name);
	}
} catch (...) {
	LogError(std::current_exception());
}

/* we don't look at files with newlines in their name */
gcc_pure
static bool
skip_path(const char *name_utf8) noexcept
{
	return std::strchr(name_utf8, '\n') != nullptr;
}

gcc_pure
bool
UpdateWalk::SkipSymlink(const Directory *directory,
			std::string_view utf8_name) const noexcept
{
#ifndef _WIN32
	const auto path_fs = storage.MapChildFS(directory->GetPath(),
						utf8_name);
	if (path_fs.IsNull())
		/* not a local file: don't skip */
		return false;

	const auto target = ReadLink(path_fs);
	if (target.IsNull())
		/* don't skip if this is not a symlink */
		return errno != EINVAL;

	if (!config.follow_inside_symlinks &&
	    !config.follow_outside_symlinks) {
		/* ignore all symlinks */
		return true;
	} else if (config.follow_inside_symlinks &&
		   config.follow_outside_symlinks) {
		/* consider all symlinks */
		return false;
	}

	if (target.IsAbsolute()) {
		/* if the symlink points to an absolute path, see if
		   that path is inside the music directory */
		const auto target_utf8 = target.ToUTF8();
		if (target_utf8.empty())
			return true;

		auto relative =
			storage.MapToRelativeUTF8(target_utf8.c_str());
		return relative.data() != nullptr
			? !config.follow_inside_symlinks
			: !config.follow_outside_symlinks;
	}

	const char *p = target.c_str();
	while (*p == '.') {
		if (p[1] == '.' && PathTraitsFS::IsSeparator(p[2])) {
			/* "../" moves to parent directory */
			directory = directory->parent;
			if (directory == nullptr) {
				/* we have moved outside the music
				   directory - skip this symlink
				   if such symlinks are not allowed */
				return !config.follow_outside_symlinks;
			}
			p += 3;
		} else if (PathTraitsFS::IsSeparator(p[1]))
			/* eliminate "./" */
			p += 2;
		else
			break;
	}

	/* we are still in the music directory, so this symlink points
	   to a song which is already in the database - skip according
	   to the follow_inside_symlinks param*/
	return !config.follow_inside_symlinks;
#else
	/* no symlink checking on WIN32 */

	(void)directory;
	(void)utf8_name;

	return false;
#endif
}

static void
LoadExcludeListOrThrow(const Storage &storage, const Directory &directory,
		       ExcludeList &exclude_list)
{
	Mutex mutex;
	auto is = InputStream::OpenReady(storage.MapUTF8(PathTraitsUTF8::Build(directory.GetPath(),
									       ".mpdignore")).c_str(),
					 mutex);
	exclude_list.Load(std::move(is));
}

static void
LoadExcludeListOrLog(const Storage &storage, const Directory &directory,
		     ExcludeList &exclude_list) noexcept
{
	try {
		LoadExcludeListOrThrow(storage, directory, exclude_list);
	} catch (...) {
		if (!IsFileNotFound(std::current_exception()))
			LogError(std::current_exception());
	}
}

bool
UpdateWalk::UpdateDirectory(Directory &directory,
			    const ExcludeList &exclude_list,
			    const StorageFileInfo &info) noexcept
{
	assert(info.IsDirectory());

	directory_set_stat(directory, info);

	/* done-set: device+inode (CIFS-safe) with realpath fallback */
	if (!DoneSetInsert(*this, info, directory.GetPath())) {
		++metric_dirs_skipped_done;
		WalkMetricsEmit("skip_dir_done", *this,
				"\"reason\":\"done_set\"");
		return true;
	}
	++metric_dirs_entered;
	if ((metric_dirs_entered % 50) == 0)
		WalkMetricsEmit("milestone", *this, "");

	std::unique_ptr<StorageDirectoryReader> reader;

	try {
		reader = storage.OpenDirectory(directory.GetPath());
	} catch (...) {
		LogError(std::current_exception());
		return false;
	}

	ExcludeList child_exclude_list(exclude_list);
	LoadExcludeListOrLog(storage, directory, child_exclude_list);

	if (!child_exclude_list.IsEmpty())
		RemoveExcludedFromDirectory(directory, child_exclude_list);

	PurgeDeletedFromDirectory(directory);

	/* Collect children, then visit real entries before symlinks.
	   Sort within class: digit, letter, other (deterministic). */
	struct ChildEntry {
		std::string name;
		StorageFileInfo info;
		bool is_symlink;
	};
	std::vector<ChildEntry> children;

	const char *name_utf8;
	while (!cancel && (name_utf8 = reader->Read()) != nullptr) {
		if (skip_path(name_utf8))
			continue;

		{
			const auto name_fs = AllocatedPath::FromUTF8(name_utf8);
			/* dont-walk: never .mpdignore-away the browse mirrors
			   (_tags/_meta). Share files/.mpdignore still lists
			   them as a CIFS safety belt; this fork indexes them. */
			if (name_fs.IsNull() ||
			    (child_exclude_list.Check(name_fs) &&
			     !IsIndexTreeName(name_utf8)))
				continue;
		}

		if (SkipSymlink(&directory, name_utf8)) {
			modified |= editor.DeleteNameIn(directory, name_utf8);
			continue;
		}

		StorageFileInfo info2;
		if (!GetInfo(*reader, info2)) {
			modified |= editor.DeleteNameIn(directory, name_utf8);
			continue;
		}

		bool is_symlink = false;
#ifndef _WIN32
		{
			const auto path_fs = storage.MapChildFS(directory.GetPath(),
							       name_utf8);
			if (!path_fs.IsNull()) {
				const auto target = ReadLink(path_fs);
				is_symlink = !target.IsNull();
			}
		}
		/* Symlink / reified alias already in the don't-walk set:
		   still queue so UpdateDirectoryChild emits a directory: row;
		   UpdateDirectory returns early and does not re-index songs. */
		if (info2.IsDirectory()) {
			const auto child_uri =
				PathTraitsUTF8::Build(directory.GetPath(),
						      name_utf8);
			if (DoneSetContains(*this, info2, child_uri.c_str())) {
				if (is_symlink)
					++metric_symlinks_skipped_done;
				else
					++metric_alias_skipped;
			}
		}
#endif
		children.push_back(ChildEntry{name_utf8, info2, is_symlink});
	}

	if (children.size() > metric_max_dir_entries)
		metric_max_dir_entries = children.size();

	std::stable_sort(children.begin(), children.end(),
			 [](const ChildEntry &a, const ChildEntry &b) {
				 if (a.is_symlink != b.is_symlink)
					 return !a.is_symlink && b.is_symlink;
				 const int ca = NameClass(a.name.c_str());
				 const int cb = NameClass(b.name.c_str());
				 if (ca != cb)
					 return ca < cb;
				 return a.name < b.name;
			 });

	for (const auto &child : children) {
		if (cancel)
			break;
		UpdateDirectoryChild(directory, child_exclude_list,
				     child.name.c_str(), child.info);
	}

	/* CIFS/smbfs: after a long album walk, browse mirrors may be missing.
	   Force a second look only at files/_tags and files/_meta. */
	{
		const char *path = directory.GetPath();
		const bool is_files =
			path != nullptr && StringIsEqual(path, "files");
		if (is_files)
			FmtWarning(update_domain,
				   "dontwalk-enddir path={} children={} cancel={}",
				   path,
				   children.size(),
				   cancel ? 1 : 0);
		if (is_files) {
			for (const char *name : {"_meta", "_tags"}) {
				bool present = false;
				{
					const ScopeDatabaseLock protect;
					present = directory.FindChild(name) != nullptr;
				}
				FmtWarning(update_domain,
					   "dontwalk-ensure {} present={}",
					   name, present ? 1 : 0);
				if (present)
					continue;
				const auto uri =
					PathTraitsUTF8::Build(path, name);
				StorageFileInfo info_retry;
				bool ok = false;
				for (int attempt = 0; attempt < 3; ++attempt) {
					if (GetInfo(storage, uri.c_str(),
						    info_retry) &&
					    info_retry.IsDirectory()) {
						ok = true;
						break;
					}
				}
				FmtWarning(update_domain,
					   "dontwalk-ensure {} getinfo={}",
					   uri.c_str(), ok ? 1 : 0);
				if (!ok)
					continue;
				UpdateDirectoryChild(directory,
						     child_exclude_list,
						     name, info_retry);
			}
		}
	}

	directory.mtime = info.mtime;

	return true;
}

inline Directory *
UpdateWalk::DirectoryMakeChildChecked(Directory &parent,
				      const char *uri_utf8,
				      std::string_view name_utf8) noexcept
{
	Directory *directory;
	{
		const ScopeDatabaseLock protect;
		directory = parent.FindChild(name_utf8);
	}

	if (directory != nullptr) {
		if (directory->IsMount())
			directory = nullptr;

		return directory;
	}

	StorageFileInfo info;
	if (!GetInfo(storage, uri_utf8, info) ||
	    FindAncestorLoop(storage, &parent, info.inode, info.device))
		return nullptr;

	if (SkipSymlink(&parent, name_utf8))
		return nullptr;

	/* if we're adding directory paths, make sure to delete filenames
	   with potentially the same name */
	{
		const ScopeDatabaseLock protect;
		Song *conflicting = parent.FindSong(name_utf8);
		if (conflicting)
			editor.DeleteSong(parent, conflicting);

		directory = parent.CreateChild(name_utf8);
	}

	directory_set_stat(*directory, info);
	return directory;
}

inline Directory *
UpdateWalk::DirectoryMakeUriParentChecked(Directory &root,
					  std::string_view _uri) noexcept
{
	Directory *directory = &root;
	StringView uri(_uri);

	while (true) {
		auto [name, rest] = uri.Split('/');
		if (rest == nullptr)
			break;

		if (!name.empty()) {
			directory = DirectoryMakeChildChecked(*directory,
							      std::string(name).c_str(),
							      name);
			if (directory == nullptr)
				break;
		}

		uri = rest;
	}

	return directory;
}

static void
LoadExcludeLists(std::forward_list<ExcludeList> &lists,
		 const Storage &storage, const Directory &directory) noexcept
{
	assert(!lists.empty());

	if (!directory.IsRoot())
		LoadExcludeLists(lists, storage, *directory.parent);

	lists.emplace_front();
	LoadExcludeListOrLog(storage, directory, lists.front());
}

static auto
LoadExcludeLists(const Storage &storage, const Directory &directory) noexcept
{
	std::forward_list<ExcludeList> lists;
	lists.emplace_front();
	LoadExcludeLists(lists, storage, directory);
	return lists;
}

inline void
UpdateWalk::UpdateUri(Directory &root, const char *uri) noexcept
try {
	Directory *parent = DirectoryMakeUriParentChecked(root, uri);
	if (parent == nullptr)
		return;

	const char *name = PathTraitsUTF8::GetBase(uri);

	if (SkipSymlink(parent, name)) {
		modified |= editor.DeleteNameIn(*parent, name);
		return;
	}

	StorageFileInfo info;
	if (!GetInfo(storage, uri, info)) {
		modified |= editor.DeleteNameIn(*parent, name);
		return;
	}

	const auto exclude_lists = LoadExcludeLists(storage, *parent);
	UpdateDirectoryChild(*parent, exclude_lists.front(), name, info);
} catch (...) {
	LogError(std::current_exception());
}

bool
UpdateWalk::Walk(Directory &root, const char *path, bool discard) noexcept
{
	walk_discard = discard;
	modified = false;
	done_ids.clear();
	done_real_paths.clear();
	metric_dirs_entered = 0;
	metric_dirs_skipped_done = 0;
	metric_alias_skipped = 0;
	metric_residual_index_skipped = 0;
	metric_symlinks_skipped_done = 0;
	metric_songs = 0;
	metric_max_dir_entries = 0;
	WalkMetricsEmit("start", *this, "");

	if (path != nullptr && !isRootDirectory(path)) {
		UpdateUri(root, path);
	} else {
		StorageFileInfo info;
		if (!GetInfo(storage, "", info))
			return false;

		if (!info.IsDirectory()) {
			FmtError(update_domain, "Not a directory: {}",
				 storage.MapUTF8(""));
			return false;
		}

		ExcludeList exclude_list;

		UpdateDirectory(root, exclude_list, info);
	}

	{
		const ScopeDatabaseLock protect;
		PurgeDanglingFromPlaylists(root);
	}

	WalkMetricsEmit("final", *this, "");
	return modified;
}
