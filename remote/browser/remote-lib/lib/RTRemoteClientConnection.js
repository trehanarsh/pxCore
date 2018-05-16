/**
 * Copyright (C) 2018 TopCoder Inc., All Rights Reserved.
 */

/**
 * The rt remote client connection
 *
 * @author      TCSCODER
 * @version     1.0
 */

const RTRemoteWBTransport = require('./RTRemoteWBTransport');
const RTRemoteObject = require('./RTRemoteObject');
const RTRemoteProtocol = require('./RTRemoteProtocol');

/**
 * the rt remote client connection class
 */
class RTRemoteClientConnection {
  /**
   * create new connection with protocol
   * @param {RTRemoteProtocol} protocol
   */
  constructor(protocol) {
    this.protocol = protocol;
  }

  /**
   * get rt remote object by object id
   * @param {string} objectId the object id
   * @return {RTRemoteObject} the returned rtRemote object
   */
  getProxyObject(objectId) {
    return new RTRemoteObject(this.protocol, objectId);
  }
}

/**
 * create websocket connection
 * @param {string} uri the websocket uri
 * @return {Promise<RTRemoteClientConnection>} the promise with connection
 */
function createWBClientConnection(uri) {
  const transport = new RTRemoteWBTransport(uri);

  // 1. create protocol, the second param is false mean protocol will open transport socket
  // connection and bind relate input/output events
  // 2. then use initialized protocol create connection
  return RTRemoteProtocol.create(transport, false).then(protocol => new RTRemoteClientConnection(protocol));
}

module.exports = {
  createWBClientConnection,
};
