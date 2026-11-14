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

#ifndef MPD_UPDATE_WALK_HXX
#define MPD_UPDATE_WALK_HXX

#include "Config.hxx"
#include "Editor.hxx"
#include "config.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

struct StorageFileInfo;
struct Directory;
struct ArchivePlugin;
struct PlaylistPlugin;
class SongEnumerator;
class ArchiveFile;
class Storage;
class ExcludeList;

/**
 * Stable directory identity for the don't-walk set.  Prefer this over
 * realpath strings: CIFS/smbfs reifies symlinks as directories with distinct
 * UTF-8 paths but the same device+inode (see todo:d6d78baa Notes).
 */
struct WalkDoneId {
	uint64_t device;
	uint64_t inode;

	constexpr bool operator==(const WalkDoneId &o) const noexcept {
		return device == o.device && inode == o.inode;
	}
};

struct WalkDoneIdHash {
	std::size_t operator()(const WalkDoneId &id) const noexcept {
		return std::hash<uint64_t>{}(id.device) ^
			(std::hash<uint64_t>{}(id.inode) << 1);
	}
};

class UpdateWalk final {
	friend void WalkMetricsEmit(const char *kind, const UpdateWalk &walk,
				    const char *extra) noexcept;
	friend bool DoneSetContains(const UpdateWalk &walk,
				    const StorageFileInfo &info,
				    const char *uri_utf8) noexcept;
	friend bool DoneSetInsert(UpdateWalk &walk,
				  const StorageFileInfo &info,
				  const char *uri_utf8) noexcept;
#ifdef ENABLE_ARCHIVE
	friend class UpdateArchiveVisitor;
#endif

	const UpdateConfig config;

	bool walk_discard;
	bool modified;

	/**
	 * Set to true by the main thread when the update thread shall
	 * cancel as quickly as possible.  Access to this flag is
	 * unprotected.
	 */
	std::atomic_bool cancel;

	Storage &storage;

	DatabaseEditor editor;

	/**
	 * Directories already entered this walk (don't-walk set).
	 * Primary key is device+inode when the storage reports them;
	 * realpath UTF-8 is a fallback when inode is unknown.
	 */
	std::unordered_set<WalkDoneId, WalkDoneIdHash> done_ids;
	std::unordered_set<std::string> done_real_paths;

	/* Tunable walk observability (also written to /tmp/mpd-walk-metrics.jsonl). */
	std::uint64_t metric_dirs_entered = 0;
	std::uint64_t metric_dirs_skipped_done = 0;
	std::uint64_t metric_alias_skipped = 0;
	std::uint64_t metric_residual_index_skipped = 0;
	std::uint64_t metric_symlinks_skipped_done = 0;
	std::uint64_t metric_songs = 0;
	std::uint64_t metric_max_dir_entries = 0;

public:
	UpdateWalk(const UpdateConfig &_config,
		   EventLoop &_loop, DatabaseListener &_listener,
		   Storage &_storage) noexcept;

	/**
	 * Cancel the current update and quit the Walk() method as
	 * soon as possible.
	 */
	void Cancel() noexcept {
		cancel = true;
	}

	/**
	 * Returns true if the database was modified.
	 */
	bool Walk(Directory &root, const char *path, bool discard) noexcept;

private:
	[[gnu::pure]]
	bool SkipSymlink(const Directory *directory,
			 std::string_view utf8_name) const noexcept;

	void RemoveExcludedFromDirectory(Directory &directory,
					 const ExcludeList &exclude_list) noexcept;

	void PurgeDeletedFromDirectory(Directory &directory) noexcept;

	/**
	 * Remove all virtual songs inside playlists whose "target"
	 * field points to a non-existing song file.
	 *
	 * It also looks up all target songs and sets their
	 * "in_playlist" field.
	 */
	void PurgeDanglingFromPlaylists(Directory &directory) noexcept;

	void UpdateSongFile2(Directory &directory,
			     const char *name, std::string_view suffix,
			     const StorageFileInfo &info) noexcept;

	bool UpdateSongFile(Directory &directory,
			    const char *name, std::string_view suffix,
			    const StorageFileInfo &info) noexcept;

	bool UpdateContainerFile(Directory &directory,
				 std::string_view name, std::string_view suffix,
				 const StorageFileInfo &info) noexcept;


#ifdef ENABLE_ARCHIVE
	void UpdateArchiveTree(ArchiveFile &archive, Directory &parent,
			       const char *name) noexcept;

	bool UpdateArchiveFile(Directory &directory,
			       std::string_view name, std::string_view suffix,
			       const StorageFileInfo &info) noexcept;

	void UpdateArchiveFile(Directory &directory, std::string_view name,
			       const StorageFileInfo &info,
			       const ArchivePlugin &plugin) noexcept;


#else
	bool UpdateArchiveFile([[maybe_unused]] Directory &directory,
			       [[maybe_unused]] const char *name,
			       [[maybe_unused]] std::string_view suffix,
			       [[maybe_unused]] const StorageFileInfo &info) noexcept {
		return false;
	}
#endif

	void UpdatePlaylistFile(Directory &directory,
				SongEnumerator &contents) noexcept;

	void UpdatePlaylistFile(Directory &parent, std::string_view name,
				const StorageFileInfo &info,
				const PlaylistPlugin &plugin) noexcept;

	bool UpdatePlaylistFile(Directory &directory,
				std::string_view name, std::string_view suffix,
				const StorageFileInfo &info) noexcept;

	bool UpdateRegularFile(Directory &directory,
			       const char *name, const StorageFileInfo &info) noexcept;

	void UpdateDirectoryChild(Directory &directory,
				  const ExcludeList &exclude_list,
				  const char *name,
				  const StorageFileInfo &info) noexcept;

	bool UpdateDirectory(Directory &directory,
			     const ExcludeList &exclude_list,
			     const StorageFileInfo &info) noexcept;

	/**
	 * Create the specified directory object if it does not exist
	 * already or if the #StorageFileInfo object indicates that it has been
	 * modified since the last update.  Returns nullptr when it
	 * exists already and is unmodified.
	 *
	 * The caller must lock the database.
	 *
	 * @param virtual_device one of the DEVICE_* constants
	 * specifying the kind of virtual directory
	 */
	Directory *MakeVirtualDirectoryIfModified(Directory &parent,
						  std::string_view name,
						  const StorageFileInfo &info,
						  unsigned virtual_device) noexcept;

	Directory *LockMakeVirtualDirectoryIfModified(Directory &parent,
						      std::string_view name,
						      const StorageFileInfo &info,
						      unsigned virtual_device) noexcept;

	Directory *DirectoryMakeChildChecked(Directory &parent,
					     const char *uri_utf8,
					     std::string_view name_utf8) noexcept;

	Directory *DirectoryMakeUriParentChecked(Directory &root,
						 std::string_view uri) noexcept;

	void UpdateUri(Directory &root, const char *uri) noexcept;
};

#endif
