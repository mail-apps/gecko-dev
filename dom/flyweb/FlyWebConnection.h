/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_FlyWebConnection_h
#define mozilla_dom_FlyWebConnection_h

#include "nsISupportsImpl.h"
#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/ErrorResult.h"
#include "nsISocketTransport.h"
#include "nsIAsyncInputStream.h"
#include "nsIAsyncOutputStream.h"
#include "nsRefPtrHashtable.h"
#include "mozilla/Variant.h"

namespace mozilla {
namespace dom {

class Promise;
struct FlyWebPublishOptions;
class FlyWebChannel;
class Response;
class FlyWebPublishedServer;

class FlyWebConnection final : public mozilla::DOMEventTargetHelper
                             , public nsIInputStreamCallback
                             , public nsIOutputStreamCallback
{
public:
  FlyWebConnection(nsPIDOMWindow* aOwner,
                   nsISocketTransport* aTransport,
                   FlyWebPublishedServer* aServer,
                   nsresult& rv);

  virtual JSObject* WrapObject(JSContext *cx, JS::Handle<JSObject*> aGivenProto) override;

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIINPUTSTREAMCALLBACK
  NS_DECL_NSIOUTPUTSTREAMCALLBACK

  IMPL_EVENT_HANDLER(message)
  IMPL_EVENT_HANDLER(fetch)
  IMPL_EVENT_HANDLER(close)

  void Close();
  void Send(const nsAString& aData);
  void GetUrl(nsAString& aUrl)
  {
    aUrl = mRoot;
  }

  FlyWebPublishedServer* GetServer()
  {
    return mServer;
  }

  nsresult MakeHttpRequest(nsIURI* aURI, FlyWebChannel* aChannel);
  void OnFetchResult(nsresult aStatus, const nsACString& aPath, Response* aResponse = nullptr);

private:
  ~FlyWebConnection();

  bool AttemptConsumeInput();
  void DispatchMessageEvent(const nsAString& aMessage);
  void DispatchFetchEvent(const nsACString& aUri);

  static NS_METHOD ReadSegmentsFunc(nsIInputStream* aIn,
                                    void* aClosure,
                                    const char* aBuffer,
                                    uint32_t aToOffset,
                                    uint32_t aCount,
                                    uint32_t* aWriteCount);
  nsresult ConsumeInput(const char*& aBuffer,
                        const char* aEnd);

  nsCOMPtr<nsISocketTransport> mTransport;
  RefPtr<FlyWebPublishedServer> mServer;
  nsCOMPtr<nsIAsyncInputStream> mInput;
  nsCOMPtr<nsIAsyncOutputStream> mOutput;

  nsTArray<Variant<nsCString, nsCOMPtr<nsIAsyncInputStream>>> mOutputBuffers;

  static const uint32_t HeaderSize = sizeof(uint32_t);

  enum InputState { eInit, eTextMessage, eFetchURL, eResponseURL, eResponseBody, eErrorURL };
  InputState mInputState;
  uint8_t mReadFrameHeader[HeaderSize];
  uint32_t mReadFrameHeaderWritten;

  uint32_t mWriteFrameRemaining;

  uint32_t mReadFrameRemaining;
  nsString mTextMessageBuffer;
  nsCString mURLBuffer;
  FlyWebChannel* mCurrentResponseChannel;

  nsString mRoot;
  bool mIsRegistered;

  nsRefPtrHashtable<nsCStringHashKey, FlyWebChannel> mChannels;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_FlyWebConnection_h
