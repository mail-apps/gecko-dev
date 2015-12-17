/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

enum FlyWebConnectionState {
  "connected",
  "closed"
};

interface FlyWebConnection : EventTarget {
  //readonly attribute DOMString serviceId;
  //readonly attribute DOMString sessionId;
  //readonly attribute DOMString? origin; // Website running other peer

  // Base-uri for other party. Null if other party doesn't have http
  // server support.
  readonly attribute DOMString? url;

  // State management
  //readonly attribute FlyWebConnectionState state;
  attribute EventHandler onclose;
  void close();

  // Communication
  //readonly attribute boolean messages; // Other party supports message protocol
                                         // using send/onmessage API below.
  //attribute BinaryType binaryType;
  attribute EventHandler onmessage;
  void send(DOMString message);
  //void send(Blob data);
  //void send(ArrayBuffer data);
  //void send(ArrayBufferView data);

  // Http server
  attribute EventHandler onfetch;
  //readonly attribute DOMString userAgent;
};