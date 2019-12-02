/* Copyright 2019 Stanford University
 * Copyright 2019 Los Alamos National Laboratory
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef LOWLEVEL_CHANNEL_DISK
#define LOWLEVEL_CHANNEL_DISK

#include "realm/transfer/channel.h"

namespace Realm {

    class FileRequest : public Request {
    public:
      int fd;
      void *mem_base; // could be source or dest
      off_t file_off;
    };
    class DiskRequest : public Request {
    public:
      int fd;
      void *mem_base; // could be source or dest
      off_t disk_off;
    };

    class FileXferDes : public XferDes {
    public:
      FileXferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
		  const std::vector<XferDesPortInfo>& inputs_info,
		  const std::vector<XferDesPortInfo>& outputs_info,
		  bool _mark_start,
		  uint64_t _max_req_size, long max_nr, int _priority,
		  XferDesFence* _complete_fence);

      ~FileXferDes()
      {
        free(file_reqs);
      }

      long get_requests(Request** requests, long nr);
      void notify_request_read_done(Request* req);
      void notify_request_write_done(Request* req);
      void flush();
    private:
      FileRequest* file_reqs;
      std::string filename;
      int fd; // The file that stores the physical instance
      //const char *buf_base;
    };

    class DiskXferDes : public XferDes {
    public:
      DiskXferDes(DmaRequest *_dma_request, NodeID _launch_node, XferDesID _guid,
		  const std::vector<XferDesPortInfo>& inputs_info,
		  const std::vector<XferDesPortInfo>& outputs_info,
		  bool _mark_start,
		  uint64_t _max_req_size, long max_nr, int _priority,
		  XferDesFence* _complete_fence);

      ~DiskXferDes() {
        free(disk_reqs);
      }

      long get_requests(Request** requests, long nr);
      void notify_request_read_done(Request* req);
      void notify_request_write_done(Request* req);
      void flush();

    private:
      int fd;
      DiskRequest* disk_reqs;
      //const char *buf_base;
    };

    class FileChannel : public Channel {
    public:
      FileChannel(long max_nr, XferDesKind _kind);
      ~FileChannel();
      long submit(Request** requests, long nr);
      void pull();
      long available();
    };

    class DiskChannel : public Channel {
    public:
      DiskChannel(long max_nr, XferDesKind _kind);
      ~DiskChannel();
      long submit(Request** requests, long nr);
      void pull();
      long available();
    };

}; // namespace Realm

#endif /*LOWLEVEL_CHANNEL_DISK*/
