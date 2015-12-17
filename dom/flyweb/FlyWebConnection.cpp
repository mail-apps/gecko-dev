/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/FlyWebConnection.h"
#include "nsString.h"
#include "mozilla/dom/FlyWebConnectionBinding.h"
#include "mozilla/dom/MessageEvent.h"
#include "jsapi.h"
#include "mozilla/dom/FlyWebService.h"
#include "mozilla/dom/FlyWebChannel.h"
#include "mozilla/dom/InternalRequest.h"
#include "mozilla/dom/Request.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/FlyWebFetchEvent.h"
#include "nsIStreamTransportService.h"

static NS_DEFINE_CID(kStreamTransportServiceCID, NS_STREAMTRANSPORTSERVICE_CID);

namespace mozilla {
namespace dom {

NS_IMPL_ISUPPORTS_INHERITED(FlyWebConnection,
                            mozilla::DOMEventTargetHelper,
                            nsIInputStreamCallback,
                            nsIOutputStreamCallback)

FlyWebConnection::FlyWebConnection(nsPIDOMWindow* aOwner,
                                   nsISocketTransport* aTransport,
                                   FlyWebPublishedServer* aServer,
                                   nsresult& rv)
  : mozilla::DOMEventTargetHelper(aOwner)
  , mTransport(aTransport)
  , mServer(aServer)
  , mInputState(eInit)
  , mReadFrameHeaderWritten(0)
  , mWriteFrameRemaining(0)
  , mReadFrameRemaining(0)
  , mCurrentResponseChannel(nullptr)
  , mIsRegistered(false)
{
  nsCOMPtr<nsIInputStream> input;
  rv = mTransport->OpenInputStream(0, 0, 0, getter_AddRefs(input));
  NS_ENSURE_SUCCESS_VOID(rv);

  mInput = do_QueryInterface(input);
  rv = mInput->AsyncWait(this, 0, 0, NS_GetCurrentThread());
  NS_ENSURE_SUCCESS_VOID(rv);

  nsCOMPtr<nsIOutputStream> output;
  rv = mTransport->OpenOutputStream(0, 0, 0, getter_AddRefs(output));
  NS_ENSURE_SUCCESS_VOID(rv);

  mOutput = do_QueryInterface(output);

  rv = FlyWebService::GetOrCreate()->RegisterConnection(this, mRoot);
  NS_ENSURE_SUCCESS_VOID(rv);

  mIsRegistered = true;
}

FlyWebConnection::~FlyWebConnection()
{
  MOZ_ASSERT(!mIsRegistered);

  if (mIsRegistered) {
    FlyWebService::GetOrCreate()->UnregisterConnection(this);
  }
}

JSObject*
FlyWebConnection::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return FlyWebConnectionBinding::Wrap(aCx, this, aGivenProto);
}

NS_METHOD
FlyWebConnection::ReadSegmentsFunc(nsIInputStream* aIn,
                                   void* aClosure,
                                   const char* aBuffer,
                                   uint32_t aToOffset,
                                   uint32_t aCount,
                                   uint32_t* aWriteCount)
{
  const char* buffer = aBuffer;
  nsresult rv = static_cast<FlyWebConnection*>(aClosure)->
    ConsumeInput(buffer, buffer + aCount);

  *aWriteCount = buffer - aBuffer;
  MOZ_ASSERT(*aWriteCount <= aCount);

  return rv;
}

nsresult
FlyWebConnection::ConsumeInput(const char*& aBuffer,
                               const char* aEnd)
{
  if (mInputState == eInit) {
    switch(aBuffer[0]) {
      case 'M': mInputState = eTextMessage; break;
      case 'F': mInputState = eFetchURL; break;
      case 'R': mInputState = eResponseURL; break;
      case 'E': mInputState = eErrorURL; break;
      default: Close(); return NS_ERROR_FAILURE;
    }
    aBuffer++;
  }

  // Reading header?
  if (mReadFrameRemaining == 0) {
    uint32_t readBytes = std::min(HeaderSize - mReadFrameHeaderWritten,
                                  static_cast<uint32_t>(aEnd - aBuffer));
    memmove(mReadFrameHeader + mReadFrameHeaderWritten, aBuffer, readBytes);

    aBuffer += readBytes;
    mReadFrameHeaderWritten += readBytes;

    if (mReadFrameHeaderWritten < HeaderSize) {
      return NS_OK;
    }

    mReadFrameHeaderWritten = 0;
    mReadFrameRemaining = NetworkEndian::readUint32(mReadFrameHeader);

    // Current message finished.
    if (mReadFrameRemaining == 0) {
      switch(mInputState) {
        case eTextMessage:
          DispatchMessageEvent(mTextMessageBuffer);
          mTextMessageBuffer.Truncate();

          mInputState = eInit;
          return NS_OK;

        case eResponseBody:
          MOZ_ASSERT(mCurrentResponseChannel);

          mCurrentResponseChannel->Finish(NS_OK);

          mCurrentResponseChannel = nullptr;
          MOZ_ASSERT(mChannels.GetWeak(mURLBuffer));
          mChannels.Remove(mURLBuffer);
          mURLBuffer.Truncate();

          mInputState = eInit;
          return NS_OK;

        default:
          MOZ_ASSERT_UNREACHABLE("should not get here unless stream data is corrupt");
      }
      Close();
      return NS_ERROR_FAILURE;
    }
  }

  uint32_t readBytes = std::min(mReadFrameRemaining,
                                static_cast<uint32_t>(aEnd - aBuffer));

  switch(mInputState) {
    case eTextMessage:
      AppendUTF8toUTF16(Substring(aBuffer, readBytes), mTextMessageBuffer);
      aBuffer += readBytes;
      mReadFrameRemaining -= readBytes;
      return NS_OK;

    case eFetchURL:
      mURLBuffer.Append(aBuffer, readBytes);
      aBuffer += readBytes;
      mReadFrameRemaining -= readBytes;
      if (mReadFrameRemaining == 0) {
        DispatchFetchEvent(mURLBuffer);
        mURLBuffer.Truncate();
        mInputState = eInit;
      }

      return NS_OK;

    case eErrorURL:
    case eResponseURL:
      mURLBuffer.Append(aBuffer, readBytes);
      aBuffer += readBytes;
      mReadFrameRemaining -= readBytes;
      if (mReadFrameRemaining == 0) {
        FlyWebChannel* channel = mChannels.GetWeak(mURLBuffer);

        MOZ_ASSERT(channel, "stream is corrupt");
        if (!channel) {
          Close();
          return NS_ERROR_FAILURE;
        }

        if (mInputState == eResponseURL) {
          mCurrentResponseChannel = channel;
          mInputState = eResponseBody;
        }
        else {
          channel->Finish(NS_BINDING_ABORTED);
          mURLBuffer.Truncate();
          mInputState = eInit;
        }
      }

      return NS_OK;

    case eResponseBody:
      MOZ_ASSERT(mCurrentResponseChannel);

      mCurrentResponseChannel->ProvideData(aBuffer, readBytes);
      aBuffer += readBytes;
      mReadFrameRemaining -= readBytes;

      return NS_OK;

    case eInit:
      MOZ_ASSERT_UNREACHABLE("should not get here");
  }


  return NS_OK;
}

NS_IMETHODIMP
FlyWebConnection::OnInputStreamReady(nsIAsyncInputStream* aStream)
{
  MOZ_ASSERT(aStream == mInput ||
             (!mOutputBuffers.IsEmpty() &&
              mOutputBuffers[0].is<nsCOMPtr<nsIAsyncInputStream>>() &&
              mOutputBuffers[0].as<nsCOMPtr<nsIAsyncInputStream>>() == aStream));

  // If the stream is the one we're waiting for in order to send data to
  // mOutput
  if (aStream != mInput) {
    OnOutputStreamReady(mOutput);
  }

  uint64_t avail;
  nsresult rv = mInput->Available(&avail);
  if (NS_FAILED(rv)) {
    DispatchTrustedEvent(NS_LITERAL_STRING("close"));
    mInput = nullptr;
    FlyWebService::GetOrCreate()->UnregisterConnection(this);
    mIsRegistered = false;
    return NS_OK;
  }

  uint32_t numRead;
  rv = mInput->ReadSegments(ReadSegmentsFunc,
                            this,
                            UINT32_MAX,
                            &numRead);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = mInput->AsyncWait(this, 0, 0, NS_GetCurrentThread());
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

void
FlyWebConnection::DispatchMessageEvent(const nsAString& aMessage)
{
  AutoJSAPI jsapi;
  if (NS_WARN_IF(!jsapi.Init(GetOwner()))) {
    return;
  }

  // Now we can turn our string into a jsval
  JSContext* cx = jsapi.cx();
  JS::Rooted<JS::Value> jsData(cx);
  JSString* jsString = JS_NewUCStringCopyN(cx,
                                           aMessage.BeginReading(),
                                           aMessage.Length());
  NS_ENSURE_TRUE_VOID(jsString);

  jsData.setString(jsString);

  // create an event that uses the MessageEvent interface,
  // which does not bubble, is not cancelable, and has no default action

  RefPtr<MessageEvent> event =
    NS_NewDOMMessageEvent(this, nullptr, nullptr);

  nsresult rv = event->InitMessageEvent(NS_LITERAL_STRING("message"), false,
                                        false, jsData, EmptyString(),
                                        EmptyString(), nullptr);
  NS_ENSURE_SUCCESS_VOID(rv);

  DispatchTrustedEvent(static_cast<Event*>(event));
}

void
FlyWebConnection::DispatchFetchEvent(const nsACString& aUri)
{
  RefPtr<InternalRequest> internalReq = new InternalRequest();
  internalReq->SetURL(aUri);

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetOwner());
  RefPtr<FlyWebFetchEvent> e = new FlyWebFetchEvent(this,
                                                    new Request(global, internalReq),
                                                    aUri);
  e->Init(this);
  e->InitEvent(NS_LITERAL_STRING("fetch"), false, false);

  DispatchTrustedEvent(e);
}

void
FlyWebConnection::Close()
{
  if (!mTransport) {
    MOZ_ASSERT(!mOutput);
    return;
  }

  mTransport->Close(NS_BINDING_ABORTED);
  mInput->Close();
  mOutput->Close();

  mTransport = nullptr;
  mOutput = nullptr;

  mTextMessageBuffer.Truncate();
  mOutputBuffers.Clear();
}

void
FlyWebConnection::Send(const nsAString& aData)
{
  if (!mOutput) {
    return;
  }

  NS_ConvertUTF16toUTF8 utf8Data(aData);

  char header[HeaderSize];
  NetworkEndian::writeUint32(header, utf8Data.Length());

  char term[HeaderSize];
  NetworkEndian::writeUint32(term, 0);

  nsCString buffer = NS_LITERAL_CSTRING("M") +
                     Substring(header, HeaderSize) +
                     utf8Data +
                     Substring(term, HeaderSize);
  mOutputBuffers.AppendElement(buffer);

  OnOutputStreamReady(mOutput);
}

nsresult
FlyWebConnection::MakeHttpRequest(nsIURI* aURI, FlyWebChannel* aChannel)
{
  if (!mOutput) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIURI> noref;
  nsresult rv = aURI->CloneIgnoringRef(getter_AddRefs(noref));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCString path;
  rv = noref->GetPath(path);
  NS_ENSURE_SUCCESS(rv, rv);

  char header[HeaderSize];
  NetworkEndian::writeUint32(header, path.Length());

  nsCString buffer = NS_LITERAL_CSTRING("F") +
                     Substring(header, HeaderSize) +
                     path;
  mOutputBuffers.AppendElement(buffer);

  mChannels.Put(path, aChannel);

  OnOutputStreamReady(mOutput);

  return NS_OK;
}

void
FlyWebConnection::OnFetchResult(nsresult aStatus,
                                const nsACString& aPath,
                                Response* aResponse)
{
  if (NS_FAILED(aStatus)) {
    char header[HeaderSize];
    NetworkEndian::writeUint32(header, aPath.Length());

    nsCString buffer = NS_LITERAL_CSTRING("E") +
                       Substring(header, HeaderSize) +
                       aPath;
    mOutputBuffers.AppendElement(buffer);

    OnOutputStreamReady(mOutput);

    return;
  }
  nsCOMPtr<nsIInputStream> body;
  aResponse->GetBody(getter_AddRefs(body));

  nsresult rv;

  nsCOMPtr<nsIAsyncInputStream> asyncBody = do_QueryInterface(body);
  if (!asyncBody) {
    nsCOMPtr<nsIStreamTransportService> sts =
      do_GetService(kStreamTransportServiceCID, &rv);
    NS_ENSURE_SUCCESS_VOID(rv);

    nsCOMPtr<nsITransport> transport;
    rv = sts->CreateInputTransport(body,
                                   /* aStartOffset */ 0,
                                   /* aReadLimit */ -1,
                                   /* aCloseWhenDone */ true,
                                   getter_AddRefs(transport));
    NS_ENSURE_SUCCESS_VOID(rv);

    nsCOMPtr<nsIInputStream> wrapper;
    rv = transport->OpenInputStream(/* aFlags */ 0,
                                    /* aSegmentSize */ 0,
                                    /* aSegmentCount */ 0,
                                    getter_AddRefs(wrapper));
    NS_ENSURE_SUCCESS_VOID(rv);

    asyncBody = do_QueryInterface(wrapper);
    MOZ_ASSERT(asyncBody);
  }

  char header[HeaderSize];
  NetworkEndian::writeUint32(header, aPath.Length());

  nsCString buffer = NS_LITERAL_CSTRING("R") +
                     Substring(header, HeaderSize) +
                     aPath;
  mOutputBuffers.AppendElement(buffer);
  mOutputBuffers.AppendElement(asyncBody);

  OnOutputStreamReady(mOutput);
}

NS_IMETHODIMP
FlyWebConnection::OnOutputStreamReady(nsIAsyncOutputStream* aStream)
{
  MOZ_ASSERT(aStream == mOutput || !mOutput);
  if (!mOutput) {
    return NS_OK;
  }

  nsresult rv;

  while (!mOutputBuffers.IsEmpty()) {
    if (mOutputBuffers[0].is<nsCString>()) {
      nsCString& buffer = mOutputBuffers[0].as<nsCString>();
      while (!buffer.IsEmpty()) {
        uint32_t written = 0;
        rv = mOutput->Write(buffer.BeginReading(),
                            buffer.Length(),
                            &written);

        buffer.Cut(0, written);

        if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
          return mOutput->AsyncWait(this, 0, 0, NS_GetCurrentThread());
        }

        if (NS_FAILED(rv)) {
          Close();
          return NS_OK;
        }
      }
      mOutputBuffers.RemoveElementAt(0);
    }
    else {
      nsCOMPtr<nsIAsyncInputStream>& stream = mOutputBuffers[0].as<nsCOMPtr<nsIAsyncInputStream>>();
      while (true) {

        if (mWriteFrameRemaining == 0) {
          uint64_t avail;
          bool streamClosed = false;
          rv = stream->Available(&avail);
          if (rv == NS_BASE_STREAM_CLOSED) {
            mOutputBuffers.RemoveElementAt(0);
            streamClosed = true;
            avail = 0;
            rv = NS_OK;
          }

          if (NS_FAILED(rv)) {
            Close();
            return NS_OK;
          }

          if (avail == 0 && !streamClosed) {
            return stream->AsyncWait(this, 0, 0, NS_GetCurrentThread());
          }

          mWriteFrameRemaining =
            avail > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(avail);

          char header[HeaderSize];
          NetworkEndian::writeUint32(header, mWriteFrameRemaining);

          uint32_t written = 0;
          rv = mOutput->Write(header, HeaderSize, &written);

          if (written < HeaderSize) {
            // Hopefully this is rare
            nsCString remainingHead(Substring(header + written,
                                              HeaderSize - written));
            mOutputBuffers.InsertElementAt(0, remainingHead);
            break;
          }

          if (streamClosed) {
            break;
          }
        }

        uint32_t written = 0;
        rv = mOutput->WriteFrom(stream, mWriteFrameRemaining, &written);

        mWriteFrameRemaining -= written;

        if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
          return mOutput->AsyncWait(this, 0, 0, NS_GetCurrentThread());
        }

        if (NS_FAILED(rv)) {
          Close();
          return NS_OK;
        }        
      }
    }
  }

  return NS_OK;
}

} // namespace dom
} // namespace mozilla
