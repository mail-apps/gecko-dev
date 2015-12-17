/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

interface FlyWebPublishedServer : EventTarget {
  //readonly attribute DOMString id;
  readonly attribute DOMString name;
  readonly attribute DOMString? category;
  readonly attribute boolean http;
  readonly attribute boolean message;
  readonly attribute DOMString? uiUrl;
  //readonly attribute deep_frozen object? data;

  // start(); // to make sure 'connect' events are not fired before a listener is registered
  void close();

  attribute EventHandler onconnect;
  attribute EventHandler onclose;

  // Temporary until we get publishing/discovery up and running
  readonly attribute long port;
};

dictionary FlyWebPublishOptions {
  DOMString category = "";
  boolean http = false;
  boolean message = false;
  DOMString? uiUrl = null; // URL to user interface. Can be different server. Makes
                           // endpoint show up in browser's "local services" UI.
                           // If relative, resolves against the root of the server.
  //object data; // JSON formatted

  // Provided by browser
  // DOMString id;
  // ArrayBuffer? cert; // null if not encrypted
  // int port;
  // DOMString deviceName;
  // DOMString origin;
};

dictionary FlyWebFilter {
  DOMString host = "";
  long port = 0;
};
