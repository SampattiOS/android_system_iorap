// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "inode2filename/inode_resolver.h"
#include "inode2filename/search_directories.h"

#include <android-base/logging.h>

#include <fstream>
#include <stdio.h>

namespace rx = rxcpp;

namespace iorap::inode2filename {

struct InodeResolver::Impl {
  Impl(InodeResolverDependencies dependencies)
    : dependencies_{std::move(dependencies)} {
    DCHECK(dependencies_.system_call != nullptr);
    data_source_ = DataSource::Create(/*downcast*/dependencies_);
  }
  Impl(InodeResolverDependencies dependencies, std::shared_ptr<DataSource> data_source)
    : dependencies_{std::move(dependencies)} {
    DCHECK(dependencies_.system_call != nullptr);
    data_source_ = std::move(data_source);
    DCHECK(data_source_ != nullptr);
  }
  InodeResolverDependencies dependencies_;
  std::shared_ptr<DataSource> data_source_;
};

InodeResolver::InodeResolver(InodeResolverDependencies dependencies)
  : impl_(new InodeResolver::Impl{std::move(dependencies)}) {
}

InodeResolver::InodeResolver(InodeResolverDependencies dependencies,
                             std::shared_ptr<DataSource> data_source)
  : impl_(new InodeResolver::Impl{std::move(dependencies), std::move(data_source)}) {
}

std::shared_ptr<InodeResolver> InodeResolver::Create(InodeResolverDependencies dependencies) {
  // TODO: proxy resolver for IPC/Out-of-Process.
  DCHECK(dependencies.process_mode == ProcessMode::kInProcessDirect);

  return std::shared_ptr<InodeResolver>{new InodeResolver{std::move(dependencies)}};
}

std::shared_ptr<InodeResolver> InodeResolver::Create(InodeResolverDependencies dependencies,
                                                     std::shared_ptr<DataSource> data_source) {
  // TODO: proxy resolver for IPC/Out-of-Process.
  DCHECK(dependencies.process_mode == ProcessMode::kInProcessDirect);

  return std::shared_ptr<InodeResolver>{
      new InodeResolver{std::move(dependencies), std::move(data_source)}};
}

rxcpp::observable<InodeResult>
    InodeResolver::FindFilenamesFromInodes(rxcpp::observable<Inode> inodes) const {

  // It's inefficient to search for inodes until the full search list is available,
  // so first reduce to a vector so we can access all the inodes simultaneously.
  return inodes.reduce(std::vector<Inode>{},
                       [](std::vector<Inode> vec, Inode inode) {
                         vec.push_back(inode);
                         return vec;
                       },
                       [](std::vector<Inode> v) {
                         return v;  // TODO: use an identity function
                       })
    .flat_map([self=shared_from_this()](std::vector<Inode> vec) {
      // All borrowed values (e.g. SystemCall) must outlive the observable.
      return self->FindFilenamesFromInodes(vec);
    }
  );
}

rxcpp::observable<InodeResult>
InodeResolver::FindFilenamesFromInodes(std::vector<Inode> inodes) const {
  const DataSource& data_source = *impl_->data_source_;
  const InodeResolverDependencies& dependencies = impl_->dependencies_;

  // Get lazy list of inodes from the data source.
  rxcpp::observable<InodeResult> all_inodes = impl_->data_source_->EmitInodes();

  // Filter it according to the source+dependency requirements.
  // Unsubscribe from 'all_inodes' early if all inodes are matched early.
  const bool needs_device_number = !data_source.ResultIncludesDeviceNumber();
  const bool needs_verification = dependencies.verify == VerifyKind::kStat;
  SearchDirectories search{impl_->dependencies_.system_call};
  return search.FilterFilenamesForSpecificInodes(all_inodes,
                                                 inodes,
                                                 needs_device_number,
                                                 needs_verification);
}

rxcpp::observable<InodeResult>
InodeResolver::EmitAll() const {
  const DataSource& data_source = *impl_->data_source_;
  const InodeResolverDependencies& dependencies = impl_->dependencies_;

  // Get lazy list of inodes from the data source.
  rxcpp::observable<InodeResult> all_inodes = impl_->data_source_->EmitInodes();

  // Apply verification and fill-in missing device numbers.
  const bool needs_device_number = !data_source.ResultIncludesDeviceNumber();
  const bool needs_verification = dependencies.verify == VerifyKind::kStat;
  SearchDirectories search{impl_->dependencies_.system_call};
  return search.EmitAllFilenames(all_inodes,
                                 needs_device_number,
                                 needs_verification);
}

InodeResolver::~InodeResolver() {
  // std::unique_ptr requires complete types, but we hide the definition in the header.
  delete impl_;
  // XX: Does this work if we just force the dtor definition into the .cc file with a unique_ptr?
}

void InodeResolver::StartRecording() {
  impl_->data_source_->StartRecording();
}

void InodeResolver::StopRecording() {
  impl_->data_source_->StopRecording();
}

// TODO: refactor more code from search_directories into this file.
// XX: do we also need a DataSink class? lets see if recording gets more complicated.

}  // namespace iorap::inode2filename