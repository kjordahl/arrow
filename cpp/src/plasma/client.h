// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef PLASMA_CLIENT_H
#define PLASMA_CLIENT_H

#include <stdbool.h>
#include <time.h>

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include "arrow/buffer.h"
#include "arrow/status.h"
#include "arrow/util/visibility.h"
#include "plasma/common.h"
#ifdef PLASMA_GPU
#include "arrow/gpu/cuda_api.h"
#endif

using arrow::Buffer;
using arrow::Status;

namespace plasma {

#define PLASMA_DEFAULT_RELEASE_DELAY 64

// Use 100MB as an overestimate of the L3 cache size.
constexpr int64_t kL3CacheSizeBytes = 100000000;

/// Object buffer data structure.
struct ObjectBuffer {
  /// The data buffer.
  std::shared_ptr<Buffer> data;
  /// The size in bytes of the data object.
  int64_t data_size;
  /// The metadata buffer.
  std::shared_ptr<Buffer> metadata;
  /// The metadata size in bytes.
  int64_t metadata_size;
  /// The device number.
  int device_num;
};

/// Configuration options for the plasma client.
struct PlasmaClientConfig {
  /// Number of release calls we wait until the object is actually released.
  /// This allows us to avoid invalidating the cpu cache on workers if objects
  /// are reused accross tasks.
  size_t release_delay;
};

struct ClientMmapTableEntry {
  /// The result of mmap for this file descriptor.
  uint8_t* pointer;
  /// The length of the memory-mapped file.
  size_t length;
  /// The number of objects in this memory-mapped file that are currently being
  /// used by the client. When this count reaches zeros, we unmap the file.
  int count;
};

struct ObjectInUseEntry;
struct ObjectRequest;
struct PlasmaObject;

class ARROW_EXPORT PlasmaClient {
 public:
  PlasmaClient();

  ~PlasmaClient();

  /// Connect to the local plasma store and plasma manager. Return
  /// the resulting connection.
  ///
  /// \param store_socket_name The name of the UNIX domain socket to use to
  ///        connect to the Plasma store.
  /// \param manager_socket_name The name of the UNIX domain socket to use to
  ///        connect to the local Plasma manager. If this is "", then this
  ///        function will not connect to a manager.
  /// \param release_delay Number of released objects that are kept around
  ///        and not evicted to avoid too many munmaps.
  /// \param num_retries number of attempts to connect to IPC socket, default 50
  /// \return The return status.
  Status Connect(const std::string& store_socket_name,
                 const std::string& manager_socket_name, int release_delay,
                 int num_retries = -1);

  /// Create an object in the Plasma Store. Any metadata for this object must be
  /// be passed in when the object is created.
  ///
  /// \param object_id The ID to use for the newly created object.
  /// \param data_size The size in bytes of the space to be allocated for this
  /// object's
  ///        data (this does not include space used for metadata).
  /// \param metadata The object's metadata. If there is no metadata, this
  /// pointer
  ///        should be NULL.
  /// \param metadata_size The size in bytes of the metadata. If there is no
  ///        metadata, this should be 0.
  /// \param data The address of the newly created object will be written here.
  /// \param device_num The number of the device where the object is being
  ///        created.
  ///        device_num = 0 corresponds to the host,
  ///        device_num = 1 corresponds to GPU0,
  ///        device_num = 2 corresponds to GPU1, etc.
  /// \return The return status.
  Status Create(const ObjectID& object_id, int64_t data_size, uint8_t* metadata,
                int64_t metadata_size, std::shared_ptr<Buffer>* data, int device_num = 0);
  /// Get some objects from the Plasma Store. This function will block until the
  /// objects have all been created and sealed in the Plasma Store or the
  /// timeout
  /// expires. The caller is responsible for releasing any retrieved objects,
  /// but
  /// the caller should not release objects that were not retrieved.
  ///
  /// \param object_ids The IDs of the objects to get.
  /// \param num_objects The number of object IDs to get.
  /// \param timeout_ms The amount of time in milliseconds to wait before this
  ///        request times out. If this value is -1, then no timeout is set.
  /// \param object_buffers An array where the results will be stored. If the
  /// data
  ///        size field is -1, then the object was not retrieved.
  /// \return The return status.
  Status Get(const ObjectID* object_ids, int64_t num_objects, int64_t timeout_ms,
             ObjectBuffer* object_buffers);

  /// Tell Plasma that the client no longer needs the object. This should be
  /// called
  /// after Get when the client is done with the object. After this call,
  /// the address returned by Get is no longer valid. This should be called
  /// once for each call to Get (with the same object ID).
  ///
  /// \param object_id The ID of the object that is no longer needed.
  /// \return The return status.
  Status Release(const ObjectID& object_id);

  /// Check if the object store contains a particular object and the object has
  /// been sealed. The result will be stored in has_object.
  ///
  /// @todo: We may want to indicate if the object has been created but not
  /// sealed.
  ///
  /// \param object_id The ID of the object whose presence we are checking.
  /// \param has_object The function will write true at this address if
  ///        the object is present and false if it is not present.
  /// \return The return status.
  Status Contains(const ObjectID& object_id, bool* has_object);

  /// Abort an unsealed object in the object store. If the abort succeeds, then
  /// it will be as if the object was never created at all. The unsealed object
  /// must have only a single reference (the one that would have been removed by
  /// calling Seal).
  ///
  /// \param object_id The ID of the object to abort.
  /// \return The return status.
  Status Abort(const ObjectID& object_id);

  /// Seal an object in the object store. The object will be immutable after
  /// this
  /// call.
  ///
  /// \param object_id The ID of the object to seal.
  /// \return The return status.
  Status Seal(const ObjectID& object_id);

  /// Delete an object from the object store. This currently assumes that the
  /// object is present, has been sealed and not used by another client. Otherwise,
  /// it is a no operation.
  ///
  /// @todo We may want to allow the deletion of objects that are not present or
  ///       haven't been sealed.
  ///
  /// \param object_id The ID of the object to delete.
  /// \return The return status.
  Status Delete(const ObjectID& object_id);

  /// Delete objects until we have freed up num_bytes bytes or there are no more
  /// released objects that can be deleted.
  ///
  /// \param num_bytes The number of bytes to try to free up.
  /// \param num_bytes_evicted Out parameter for total number of bytes of space
  /// retrieved.
  /// \return The return status.
  Status Evict(int64_t num_bytes, int64_t& num_bytes_evicted);

  /// Compute the hash of an object in the object store.
  ///
  /// \param object_id The ID of the object we want to hash.
  /// \param digest A pointer at which to return the hash digest of the object.
  ///        The pointer must have at least kDigestSize bytes allocated.
  /// \return The return status.
  Status Hash(const ObjectID& object_id, uint8_t* digest);

  /// Subscribe to notifications when objects are sealed in the object store.
  /// Whenever an object is sealed, a message will be written to the client
  /// socket that is returned by this method.
  ///
  /// \param fd Out parameter for the file descriptor the client should use to
  /// read notifications
  ///         from the object store about sealed objects.
  /// \return The return status.
  Status Subscribe(int* fd);

  /// Receive next object notification for this client if Subscribe has been called.
  ///
  /// \param fd The file descriptor we are reading the notification from.
  /// \param object_id Out parameter, the object_id of the object that was sealed.
  /// \param data_size Out parameter, the data size of the object that was sealed.
  /// \param metadata_size Out parameter, the metadata size of the object that was sealed.
  /// \return The return status.
  Status GetNotification(int fd, ObjectID* object_id, int64_t* data_size,
                         int64_t* metadata_size);

  /// Disconnect from the local plasma instance, including the local store and
  /// manager.
  ///
  /// \return The return status.
  Status Disconnect();

  /// Attempt to initiate the transfer of some objects from remote Plasma
  /// Stores.
  /// This method does not guarantee that the fetched objects will arrive
  /// locally.
  ///
  /// For an object that is available in the local Plasma Store, this method
  /// will
  /// not do anything. For an object that is not available locally, it will
  /// check
  /// if the object are already being fetched. If so, it will not do anything.
  /// If
  /// not, it will query the object table for a list of Plasma Managers that
  /// have
  /// the object. The object table will return a non-empty list, and this Plasma
  /// Manager will attempt to initiate transfers from one of those Plasma
  /// Managers.
  ///
  /// This function is non-blocking.
  ///
  /// This method is idempotent in the sense that it is ok to call it multiple
  /// times.
  ///
  /// \param num_object_ids The number of object IDs fetch is being called on.
  /// \param object_ids The IDs of the objects that fetch is being called on.
  /// \return The return status.
  Status Fetch(int num_object_ids, const ObjectID* object_ids);

  /// Wait for (1) a specified number of objects to be available (sealed) in the
  /// local Plasma Store or in a remote Plasma Store, or (2) for a timeout to
  /// expire. This is a blocking call.
  ///
  /// \param num_object_requests Size of the object_requests array.
  /// \param object_requests Object event array. Each element contains a request
  ///        for a particular object_id. The type of request is specified in the
  ///        "type" field.
  ///        - A PLASMA_QUERY_LOCAL request is satisfied when object_id becomes
  ///          available in the local Plasma Store. In this case, this function
  ///          sets the "status" field to ObjectStatus_Local. Note, if the
  ///          status
  ///          is not ObjectStatus_Local, it will be ObjectStatus_Nonexistent,
  ///          but it may exist elsewhere in the system.
  ///        - A PLASMA_QUERY_ANYWHERE request is satisfied when object_id
  ///        becomes
  ///          available either at the local Plasma Store or on a remote Plasma
  ///          Store. In this case, the functions sets the "status" field to
  ///          ObjectStatus_Local or ObjectStatus_Remote.
  /// \param num_ready_objects The number of requests in object_requests array
  /// that
  ///        must be satisfied before the function returns, unless it timeouts.
  ///        The num_ready_objects should be no larger than num_object_requests.
  /// \param timeout_ms Timeout value in milliseconds. If this timeout expires
  ///        before min_num_ready_objects of requests are satisfied, the
  ///        function
  ///        returns.
  /// \param num_objects_ready Out parameter for number of satisfied requests in
  ///        the object_requests list. If the returned number is less than
  ///        min_num_ready_objects this means that timeout expired.
  /// \return The return status.
  Status Wait(int64_t num_object_requests, ObjectRequest* object_requests,
              int num_ready_objects, int64_t timeout_ms, int* num_objects_ready);

  /// Transfer local object to a different plasma manager.
  ///
  /// \param addr IP address of the plasma manager we are transfering to.
  /// \param port Port of the plasma manager we are transfering to.
  /// \param object_id ObjectID of the object we are transfering.
  /// \return The return status.
  Status Transfer(const char* addr, int port, const ObjectID& object_id);

  /// Return the status of a given object. This method may query the object
  /// table.
  ///
  /// \param object_id The ID of the object whose status we query.
  /// \param object_status Out parameter for object status. Can take the
  ///         following values.
  ///         - PLASMA_CLIENT_LOCAL, if object is stored in the local Plasma
  ///         Store.
  ///           has been already scheduled by the Plasma Manager.
  ///         - PLASMA_CLIENT_TRANSFER, if the object is either currently being
  ///           transferred or just scheduled.
  ///         - PLASMA_CLIENT_REMOTE, if the object is stored at a remote
  ///           Plasma Store.
  ///         - PLASMA_CLIENT_DOES_NOT_EXIST, if the object doesn’t exist in the
  ///           system.
  /// \return The return status.
  Status Info(const ObjectID& object_id, int* object_status);

  /// Get the file descriptor for the socket connection to the plasma manager.
  ///
  /// \return The file descriptor for the manager connection. If there is no
  ///         connection to the manager, this is -1.
  int get_manager_fd() const;

 private:
  /// This is a helper method for unmapping objects for which all references have
  /// gone out of scope, either by calling Release or Abort.
  ///
  /// @param object_id The object ID whose data we should unmap.
  Status UnmapObject(const ObjectID& object_id);

  /// This is a helper method that flushes all pending release calls to the
  /// store.
  Status FlushReleaseHistory();

  Status PerformRelease(const ObjectID& object_id);

  uint8_t* lookup_or_mmap(int fd, int store_fd_val, int64_t map_size);

  uint8_t* lookup_mmapped_file(int store_fd_val);

  void increment_object_count(const ObjectID& object_id, PlasmaObject* object,
                              bool is_sealed);

  /// File descriptor of the Unix domain socket that connects to the store.
  int store_conn_;
  /// File descriptor of the Unix domain socket that connects to the manager.
  int manager_conn_;
  /// Table of dlmalloc buffer files that have been memory mapped so far. This
  /// is a hash table mapping a file descriptor to a struct containing the
  /// address of the corresponding memory-mapped file.
  std::unordered_map<int, ClientMmapTableEntry> mmap_table_;
  /// A hash table of the object IDs that are currently being used by this
  /// client.
  std::unordered_map<ObjectID, std::unique_ptr<ObjectInUseEntry>, UniqueIDHasher>
      objects_in_use_;
  /// Object IDs of the last few release calls. This is a deque and
  /// is used to delay releasing objects to see if they can be reused by
  /// subsequent tasks so we do not unneccessarily invalidate cpu caches.
  /// TODO(pcm): replace this with a proper lru cache using the size of the L3
  /// cache.
  std::deque<ObjectID> release_history_;
  /// The number of bytes in the combined objects that are held in the release
  /// history doubly-linked list. If this is too large then the client starts
  /// releasing objects.
  int64_t in_use_object_bytes_;
  /// Configuration options for the plasma client.
  PlasmaClientConfig config_;
  /// The amount of memory available to the Plasma store. The client needs this
  /// information to make sure that it does not delay in releasing so much
  /// memory that the store is unable to evict enough objects to free up space.
  int64_t store_capacity_;
#ifdef PLASMA_GPU
  /// Cuda Device Manager.
  arrow::gpu::CudaDeviceManager* manager_;
#endif
};

}  // namespace plasma

#endif  // PLASMA_CLIENT_H
