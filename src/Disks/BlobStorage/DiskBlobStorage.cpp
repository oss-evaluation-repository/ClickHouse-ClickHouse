#include <Disks/BlobStorage/DiskBlobStorage.h>

#if USE_AZURE_BLOB_STORAGE

#include <iostream>
#include <random>
#include <common/logger_useful.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BLOB_STORAGE_ERROR;
}


DiskBlobStorageSettings::DiskBlobStorageSettings(
    UInt64 max_single_part_upload_size_,
    UInt64 min_bytes_for_seek_,
    int thread_pool_size_,
    int objects_chunk_size_to_delete_) :
    max_single_part_upload_size(max_single_part_upload_size_),
    min_bytes_for_seek(min_bytes_for_seek_),
    thread_pool_size(thread_pool_size_),
    objects_chunk_size_to_delete(objects_chunk_size_to_delete_) {}


class BlobStoragePathKeeper : public RemoteFSPathKeeper
{
public:
    BlobStoragePathKeeper(size_t chunk_limit_) : RemoteFSPathKeeper(chunk_limit_) {}

    void addPath(const String & path) override
    {
        paths.push_back(path);
    }

// TODO: maybe introduce a getter?
// private:
    std::vector<String> paths;
};


class ReadIndirectBufferFromBlobStorage final : public ReadIndirectBufferFromRemoteFS<ReadBufferFromBlobStorage>
{
public:
    ReadIndirectBufferFromBlobStorage(
        std::shared_ptr<Azure::Storage::Blobs::BlobContainerClient> blob_container_client_,
        IDiskRemote::Metadata metadata_,
        size_t buf_size_) :
        ReadIndirectBufferFromRemoteFS<ReadBufferFromBlobStorage>(metadata_),
        blob_container_client(blob_container_client_),
        buf_size(buf_size_)
    {}

    std::unique_ptr<ReadBufferFromBlobStorage> createReadBuffer(const String & path) override
    {
        return std::make_unique<ReadBufferFromBlobStorage>(blob_container_client, metadata.remote_fs_root_path + path, buf_size);
    }

private:
    std::shared_ptr<Azure::Storage::Blobs::BlobContainerClient> blob_container_client;
    size_t buf_size;
};


DiskBlobStorage::DiskBlobStorage(
    const String & name_,
    const String & metadata_path_,
    std::shared_ptr<Azure::Storage::Blobs::BlobContainerClient> blob_container_client_,
    SettingsPtr settings_,
    GetDiskSettings settings_getter_) :
    IDiskRemote(name_, "" /* TODO: shall we provide a config for this path? */, metadata_path_, "DiskBlobStorage", settings_->thread_pool_size),
    blob_container_client(blob_container_client_),
    current_settings(std::move(settings_)),
    settings_getter(settings_getter_) {}


std::unique_ptr<ReadBufferFromFileBase> DiskBlobStorage::readFile(
    const String & path,
    size_t buf_size,
    size_t /*estimated_size*/,
    size_t /*direct_io_threshold*/,
    size_t /*mmap_threshold*/,
    MMappedFileCache *) const
{
    auto metadata = readMeta(path);

    LOG_DEBUG(log, "Read from file by path: {}", backQuote(metadata_path + path));

    auto reader = std::make_unique<ReadIndirectBufferFromBlobStorage>(
        blob_container_client, metadata, buf_size);

    return std::make_unique<SeekAvoidingReadBuffer>(std::move(reader), current_settings.get()->min_bytes_for_seek);
}


std::unique_ptr<WriteBufferFromFileBase> DiskBlobStorage::writeFile(
    const String & path,
    size_t buf_size,
    WriteMode mode)
{
    auto metadata = readOrCreateMetaForWriting(path, mode);
    auto blob_path = path; // TODO: maybe use getRandomName() or modify the path (now it contains the tmp_* directory part)

    LOG_DEBUG(log, "{} to file by path: {}. Blob Storage path: {}",
        mode == WriteMode::Rewrite ? "Write" : "Append", backQuote(metadata_path + path), remote_fs_root_path + blob_path);

    auto buffer = std::make_unique<WriteBufferFromBlobStorage>(
        blob_container_client,
        metadata.remote_fs_root_path + blob_path,
        current_settings.get()->max_single_part_upload_size,
        buf_size);

    return std::make_unique<WriteIndirectBufferFromRemoteFS<WriteBufferFromBlobStorage>>(std::move(buffer), std::move(metadata), blob_path);
}


DiskType::Type DiskBlobStorage::getType() const
{
    return DiskType::Type::BlobStorage;
}


bool DiskBlobStorage::supportZeroCopyReplication() const
{
    return true;
}


bool DiskBlobStorage::checkUniqueId(const String & id) const
{
    Azure::Storage::Blobs::ListBlobsOptions blobs_list_options;
    blobs_list_options.Prefix = id;
    blobs_list_options.PageSizeHint = 1;

    // TODO: does it return at most 5k blobs? Do we ever need the continuation token?
    auto blobs_list_response = blob_container_client->ListBlobs(blobs_list_options);
    auto blobs_list = blobs_list_response.Blobs;

    for (auto blob : blobs_list)
    {
        if (id == blob.Name)
            return true;
    }

    return false;
}


void DiskBlobStorage::removeFromRemoteFS(RemoteFSPathKeeperPtr fs_paths_keeper)
{
    auto * paths_keeper = dynamic_cast<BlobStoragePathKeeper *>(fs_paths_keeper.get());

    if (paths_keeper)
    {
        for (auto path : paths_keeper->paths)
        {
            if (!blob_container_client->DeleteBlob(path).Value.Deleted)
                throw Exception(ErrorCodes::BLOB_STORAGE_ERROR, "Failed to delete file in Blob Storage: {}", path);
        }
    }
}


RemoteFSPathKeeperPtr DiskBlobStorage::createFSPathKeeper() const
{
    return std::make_shared<BlobStoragePathKeeper>(current_settings.get()->objects_chunk_size_to_delete);
}

// NOTE: applyNewSettings - direct copy-paste from DiskS3
void DiskBlobStorage::applyNewSettings(const Poco::Util::AbstractConfiguration & config, ContextPtr context, const String &, const DisksMap &)
{
    auto new_settings = settings_getter(config, "storage_configuration.disks." + name, context);

    current_settings.set(std::move(new_settings));

    if (AsyncExecutor * exec = dynamic_cast<AsyncExecutor*>(&getExecutor()))
        exec->setMaxThreads(current_settings.get()->thread_pool_size);
}

}

#endif
