/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/FlyWebService.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/FlyWebPublishedServer.h"
#include "nsISocketTransportService.h"
#include "mozilla/dom/FlyWebChannel.h"
#include "nsIUUIDGenerator.h"
#include "nsStandardURL.h"
#include "mozilla/Services.h"
#include "nsISupportsPrimitives.h"

namespace mozilla {
namespace dom {

struct FlyWebPublishOptions;

static StaticRefPtr<FlyWebService> gFlyWebService;

NS_IMPL_ISUPPORTS(FlyWebService,
                  nsIProtocolHandler,
                  nsIObserver)

FlyWebService::FlyWebService()
{
  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->AddObserver(this, "inner-window-destroyed", false);
  }
}

FlyWebService*
FlyWebService::GetExisting()
{
  return gFlyWebService;
}

FlyWebService*
FlyWebService::GetOrCreate()
{
  if (!gFlyWebService) {
    gFlyWebService = new FlyWebService();
  }
  return gFlyWebService;
}

already_AddRefed<Promise>
FlyWebService::PublishServer(const nsAString& aName,
                             const FlyWebPublishOptions& aOptions,
                             nsPIDOMWindow* aWindow,
                             ErrorResult& aRv)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aWindow);
  RefPtr<Promise> promise = Promise::Create(global, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  RefPtr<FlyWebPublishedServer> server =
    new FlyWebPublishedServer(aWindow, aName, aOptions);

  nsresult rv;
  nsCOMPtr<nsIServerSocket> serverSocket =
    do_CreateInstance("@mozilla.org/network/server-socket;1", &rv);
  if (NS_SUCCEEDED(rv)) {
    rv = serverSocket->Init(-1, false, -1);
  }
  if (NS_SUCCEEDED(rv)) {
    rv = serverSocket->AsyncListen(server);
  }
  if (NS_FAILED(rv)) {
    if (serverSocket) {
      serverSocket->Close();
    }
    promise->MaybeReject(rv);

    return promise.forget();
  }

  server->SetServerSocket(serverSocket);

  mServers.AppendElement(server);
  promise->MaybeResolve(server);

  return promise.forget();
}

already_AddRefed<Promise>
FlyWebService::ConnectToServer(const FlyWebFilter& aFilter,
                               nsPIDOMWindow* aWindow,
                               ErrorResult& aRv)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aWindow);
  RefPtr<Promise> promise = Promise::Create(global, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  RefPtr<FlyWebConnection> connection;

  nsCOMPtr<nsISocketTransportService> sts =
    do_GetService("@mozilla.org/network/socket-transport-service;1");

  nsCOMPtr<nsISocketTransport> transport;
  nsresult rv = sts->CreateTransport(nullptr, 0,
                                     NS_ConvertUTF16toUTF8(aFilter.mHost),
                                     aFilter.mPort,
                                     nullptr, getter_AddRefs(transport));

  if (NS_SUCCEEDED(rv)) {
    connection = new FlyWebConnection(aWindow, transport, nullptr, rv);
  }
  if (NS_FAILED(rv)) {
    if (transport) {
      transport->Close(rv);
    }
    promise->MaybeReject(rv);

    return promise.forget();
  }

  promise->MaybeResolve(connection);

  return promise.forget();
}

/* readonly attribute ACString scheme; */
NS_IMETHODIMP
FlyWebService::GetScheme(nsACString & aScheme)
{
  aScheme.AssignLiteral("flyweb");
  return NS_OK;
}

/* readonly attribute long defaultPort; */
NS_IMETHODIMP
FlyWebService::GetDefaultPort(int32_t *aDefaultPort)
{
  *aDefaultPort = -1;
  return NS_OK;
}

NS_IMETHODIMP
FlyWebService::GetProtocolFlags(uint32_t *aProtocolFlags)
{
  // XXX fix security model
  *aProtocolFlags = URI_STD | URI_LOADABLE_BY_ANYONE | URI_FETCHABLE_BY_ANYONE;
  return NS_OK;
}

NS_IMETHODIMP
FlyWebService::NewURI(const nsACString& aSpec,
                      const char* aCharset,
                      nsIURI* aBaseURI,
                      nsIURI** aURI)
{
  RefPtr<nsStandardURL> url = new nsStandardURL();

  nsresult rv =
    url->Init(nsIStandardURL::URLTYPE_STANDARD, -1, aSpec, aCharset, aBaseURI);
  NS_ENSURE_SUCCESS(rv, rv);

  url.forget(aURI);

  return NS_OK;
}

NS_IMETHODIMP
FlyWebService::NewChannel2(nsIURI* aURI,
                           nsILoadInfo* aLoadInfo,
                           nsIChannel** aChannel)
{
  NS_ADDREF(*aChannel = new FlyWebChannel(aURI, aLoadInfo));
  return NS_OK;
}

NS_IMETHODIMP
FlyWebService::NewChannel(nsIURI* aURI, nsIChannel** aChannel)
{
  return NewChannel2(aURI, nullptr, aChannel);
}

NS_IMETHODIMP
FlyWebService::AllowPort(int32_t port, const char* scheme, bool* _retval)
{
  *_retval = false;
  return NS_OK;
}

NS_IMETHODIMP
FlyWebService::Observe(nsISupports* aSubject, const char* aTopic,
                       const char16_t* aData)
{
  if (strcmp(aTopic, "inner-window-destroyed")) {
    return NS_OK;
  }

  nsCOMPtr<nsISupportsPRUint64> wrapper = do_QueryInterface(aSubject);
  NS_ENSURE_TRUE(wrapper, NS_ERROR_FAILURE);

  uint64_t innerID;
  nsresult rv = wrapper->GetData(&innerID);
  NS_ENSURE_SUCCESS(rv, rv);

  for (FlyWebConnection* connection : mConnections) {
    nsPIDOMWindow* win = connection->GetOwner();
    if (win && win->WindowID() == innerID) {
      connection->Close();
    }
  }

  for (FlyWebPublishedServer* server : mServers) {
    nsPIDOMWindow* win = server->GetOwner();
    if (win && win->WindowID() == innerID) {
      server->Close();
    }
  }

  return NS_OK;
}

nsresult
FlyWebService::RegisterConnection(FlyWebConnection* aConnection,
                                  nsAString& aRoot)
{
  nsresult rv;
  nsCOMPtr<nsIUUIDGenerator> uuidgen =
    do_GetService("@mozilla.org/uuid-generator;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsID id;
  rv = uuidgen->GenerateUUIDInPlace(&id);
  NS_ENSURE_SUCCESS(rv, rv);

  char uuidChars[NSID_LENGTH];
  id.ToProvidedString(uuidChars);
  nsCString prePath = NS_LITERAL_CSTRING("flyweb://") +
                      Substring(uuidChars + 1, uuidChars + NSID_LENGTH - 2);
  mConnectionRoots.Put(prePath, aConnection);
  aRoot = NS_ConvertUTF8toUTF16(prePath + NS_LITERAL_CSTRING("/"));

  mConnections.AppendElement(aConnection);

  return NS_OK;
}

void
FlyWebService::UnregisterConnection(FlyWebConnection* aConnection)
{
  nsString root;
  aConnection->GetUrl(root);

  NS_ConvertUTF16toUTF8 prePath(StringHead(root, root.Length() - 1));
  MOZ_ASSERT(mConnectionRoots.Get(prePath) == aConnection);
  mConnectionRoots.Remove(prePath);

  DebugOnly<bool> removed = mConnections.RemoveElement(aConnection);
  MOZ_ASSERT(removed);
}


FlyWebConnection*
FlyWebService::GetConnection(nsIURI* aURI)
{
  nsCString prePath;
  nsresult rv = aURI->GetPrePath(prePath);
  NS_ENSURE_SUCCESS(rv, nullptr);

  return mConnectionRoots.Get(prePath);
}

void
FlyWebService::UnregisterServer(FlyWebPublishedServer* aServer)
{
  DebugOnly<bool> removed = mServers.RemoveElement(aServer);
  MOZ_ASSERT(removed);
}

bool
FlyWebService::HasConnectionOrServer(uint64_t aWindowID)
{
  for (FlyWebConnection* connection : mConnections) {
    nsPIDOMWindow* win = connection->GetOwner();
    if (win && win->WindowID() == aWindowID) {
      return true;
    }
  }

  for (FlyWebPublishedServer* server : mServers) {
    nsPIDOMWindow* win = server->GetOwner();
    if (win && win->WindowID() == aWindowID) {
      return true;
    }
  }

  return false;
}

void
FlyWebService::CloseConnectionsForServer(FlyWebPublishedServer* aServer)
{
  for (FlyWebConnection* connection : mConnections) {
    if (connection->GetServer() == aServer) {
      return connection->Close();
    }
  }
}


} // namespace dom
} // namespace mozilla
