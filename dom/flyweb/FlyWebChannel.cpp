/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/FlyWebChannel.h"
#include "nsIPipe.h"
#include "mozilla/dom/FlyWebService.h"
#include "mozilla/dom/FlyWebConnection.h"

namespace mozilla {
namespace dom {

NS_IMPL_ISUPPORTS_INHERITED(FlyWebChannel,
                            nsBaseChannel,
                            nsIOutputStreamCallback)

FlyWebChannel::FlyWebChannel(nsIURI* aURI, nsILoadInfo* aLoadInfo)
  : mPendingClose(false)
{
  SetURI(aURI);
  mLoadInfo = aLoadInfo;
}

nsresult
FlyWebChannel::OpenContentStream(bool async,
                                 nsIInputStream **stream,
                                 nsIChannel** channel)
{
  NS_ENSURE_TRUE(async, NS_ERROR_NOT_IMPLEMENTED);

  nsCOMPtr<nsIOutputStream> output;
  nsresult rv = NS_NewPipe(stream,
                           getter_AddRefs(output),
                           0, 0,
                           true,   // nonBlockingInput
                           true);  // nonBlockingOutput
  NS_ENSURE_SUCCESS(rv, rv);

  mOutput = do_QueryInterface(output);

  FlyWebConnection* connection =
    FlyWebService::GetOrCreate()->GetConnection(mURI);

  if (!connection) {
    mOutput->CloseWithStatus(NS_BINDING_ABORTED);
    return NS_OK;
  }

  return connection->MakeHttpRequest(mURI, this);
}

void
FlyWebChannel::ProvideData(const char* aBuffer, uint32_t aCount)
{
  if (!mOutput) {
    return;
  }

  if (!mOutputBuffer.IsEmpty()) {
    mOutputBuffer.Append(aBuffer, aCount);
    return;
  }

  while (aCount > 0) {
    uint32_t written;
    nsresult rv = mOutput->Write(aBuffer, aCount, &written);

    aBuffer += written;
    aCount -= written;

    if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
      mOutputBuffer.Append(aBuffer, aCount);
      mOutput->AsyncWait(this, 0, 0, NS_GetCurrentThread());
      return;
    }

    if (NS_FAILED(rv)) {
      Finish(rv);
      return;
    }
  }
}

void
FlyWebChannel::Finish(nsresult aStatus)
{
  if (NS_SUCCEEDED(aStatus) && !mOutputBuffer.IsEmpty()) {
    mPendingClose = true;
    return;
  }

  mOutput->CloseWithStatus(aStatus);
  mOutput = nullptr;
}

NS_IMETHODIMP
FlyWebChannel::OnOutputStreamReady(nsIAsyncOutputStream* aStream)
{
  MOZ_ASSERT(aStream == mOutput || !mOutput);
  MOZ_ASSERT(!mOutputBuffer.IsEmpty() || !mOutput);

  if (!mOutput) {
    return NS_OK;
  }

  while (!mOutputBuffer.IsEmpty()) {
    uint32_t written = 0;
    nsresult rv = mOutput->Write(mOutputBuffer.BeginReading(),
                                 mOutputBuffer.Length(),
                                 &written);

    mOutputBuffer.Cut(0, written);

    if (rv == NS_BASE_STREAM_WOULD_BLOCK) {
      mOutput->AsyncWait(this, 0, 0, NS_GetCurrentThread());
      return NS_OK;
    }

    if (NS_FAILED(rv)) {
      Finish(rv);
      return NS_OK;
    }
  }

  if (mPendingClose) {
    mOutput->CloseWithStatus(NS_OK);
    mOutput = nullptr;
    mPendingClose = false;
  }

  return NS_OK;
}

} // namespace dom
} // namespace mozilla
