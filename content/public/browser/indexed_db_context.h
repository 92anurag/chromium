// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_INDEXED_DB_CONTEXT_H_
#define CONTENT_PUBLIC_BROWSER_INDEXED_DB_CONTEXT_H_
#pragma once

#include <vector>

#include "base/basictypes.h"
#include "base/memory/ref_counted.h"
#include "content/common/content_export.h"

class GURL;

namespace base {
class Time;
}

namespace content {

class BrowserContext;

// Represents the per-BrowserContext IndexedDB data.
// Call these methods only on the WebKit thread.
class IndexedDBContext : public base::RefCountedThreadSafe<IndexedDBContext> {
 public:
  virtual ~IndexedDBContext() {}

  CONTENT_EXPORT static IndexedDBContext* GetForBrowserContext(
      BrowserContext* browser_context);

  // Methods used in response to QuotaManager requests.
  virtual std::vector<GURL> GetAllOrigins() = 0;
  virtual int64 GetOriginDiskUsage(const GURL& origin_url) = 0;
  virtual base::Time GetOriginLastModified(const GURL& origin_url) = 0;

  // Deletes all indexed db files for the given origin.
  virtual void DeleteForOrigin(const GURL& origin_url) = 0;

  // Get the file name of the local storage file for the given origin.
  virtual FilePath GetFilePathForTesting(const string16& origin_id) const = 0;
};

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_INDEXED_DB_CONTEXT_H_
