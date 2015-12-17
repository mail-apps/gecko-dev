/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FlyWebChannel_h
#define mozilla_dom_FlyWebChannel_h

#include "nsISupportsImpl.h"
#include "nsBaseChannel.h"
#include "nsIURI.h"
#include "nsILoadInfo.h"
#include "nsIAsyncOutputStream.h"

namespace mozilla {
namespace dom {

class FlyWebChannel final : public nsBaseChannel
                          , public nsIOutputStreamCallback
{
public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIOUTPUTSTREAMCALLBACK

  FlyWebChannel(nsIURI* aURI, nsILoadInfo* aLoadInfo);

  // Implemented hooks for nsBaseChannel
  virtual nsresult OpenContentStream(bool async, nsIInputStream **stream,
                                     nsIChannel** channel) override;

  void ProvideData(const char* aBuffer, uint32_t aCount);
  void Finish(nsresult aStatus);

private:
  ~FlyWebChannel() {};

  nsCOMPtr<nsIAsyncOutputStream> mOutput;
  nsCString mOutputBuffer;
  bool mPendingClose;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_FlyWebChannel_h
