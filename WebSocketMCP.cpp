#include "WebSocketMCP.h"

// Static instance pointer initialization
WebSocketMCP* WebSocketMCP::instance = nullptr;

// Static constant definitions
const int WebSocketMCP::INITIAL_BACKOFF;
const int WebSocketMCP::MAX_BACKOFF;
const int WebSocketMCP::PING_INTERVAL;
const int WebSocketMCP::DISCONNECT_TIMEOUT;

WebSocketMCP::WebSocketMCP() : connected(false), lastReconnectAttempt(0), 
                              currentBackoff(INITIAL_BACKOFF), reconnectAttempt(0) {
  // Set static instance pointer
  instance = this;
  connectionCallback = nullptr;
}

bool WebSocketMCP::begin(const char *mcpEndpoint,  ConnectionCallback connCb) {
  // Store callback function
  connectionCallback = connCb;
  
  // Parse WebSocket URL
  String url = String(mcpEndpoint);
  int protocolPos = url.indexOf("://");
  String protocol = url.substring(0, protocolPos);
  
  String remaining = url.substring(protocolPos + 3);
  int pathPos = remaining.indexOf('/');
  
  String host;
  String path = "/";
  int port = 80;
  
  if (pathPos >= 0) {
    host = remaining.substring(0, pathPos);
    path = remaining.substring(pathPos);
  } else {
    host = remaining;
  }
  
  // Check port
  int portPos = host.indexOf(':');
  if (portPos >= 0) {
    port = host.substring(portPos + 1).toInt();
    host = host.substring(0, portPos);
  } else {
    // Set default port based on protocol
    if (protocol == "ws") {
      port = 80;
    } else if (protocol == "wss") {
      port = 443;
    }
  }
  
  // Configure WebSocket client
  if (protocol == "wss") {
    // webSocket.beginSSL(host.c_str(), port, path.c_str());
#if defined(ESP8266)
    // ESP8266 requires 2 extra empty parameters "", ""
    webSocket.beginSSL(host.c_str(), port, path.c_str(), NULL, "arduino");
#else
    // ESP32 uses normal syntax
    webSocket.beginSSL(host.c_str(), port, path.c_str());
#endif    
  } else {
    webSocket.begin(host.c_str(), port, path.c_str());
  }
  
  // Set disconnect timeout to 60 seconds
  // webSocket.setReconnectInterval(DISCONNECT_TIMEOUT);
  webSocket.enableHeartbeat(PING_INTERVAL, PING_INTERVAL, DISCONNECT_TIMEOUT);
  
  // Register event callback
  webSocket.onEvent(webSocketEvent);
  
  Serial.println("[xiaozhi-mcp] Connecting to WebSocket server: " + url);
  return true;
}

void WebSocketMCP::webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  // Ensure instance exists
  if (!instance) {
    return;
  }
  
  switch (type) {
    case WStype_DISCONNECTED:
      if (instance->connected) {
        instance->connected = false;
        Serial.println("[xiaozhi-mcp] WebSocket disconnected");
        if (instance->connectionCallback) {
          instance->connectionCallback(false);
        }
      }
      break;
      
    case WStype_CONNECTED:
      {
        instance->connected = true;
        instance->resetReconnectParams();
        Serial.println("[xiaozhi-mcp] WebSocket connected");
        if (instance->connectionCallback) {
          instance->connectionCallback(true);
        }
      }
      break;
      
    case WStype_TEXT:
      {
        // Received WebSocket text message, process JSON-RPC request
        String message = String((char *)payload);
        instance->handleJsonRpcMessage(message);
      }
      break;
      
    case WStype_BIN:
      Serial.println("[xiaozhi-mcp] Received binary data, length: " + String(length));
      break;
      
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      break;
  }
}

bool WebSocketMCP::sendMessage(const String &message) {
  if (!connected) {
    Serial.println("[xiaozhi-mcp] Not connected to WebSocket server, cannot send message");
    return false;
  }
  // Send text message to WebSocket server (stdin equivalent)
  Serial.println("[xiaozhi-mcp] Sending message: " + message);
  String msg = message;
  webSocket.sendTXT(msg);
  return true;
}

void WebSocketMCP::loop() {
  // Handle WebSocket connection
  webSocket.loop();
  
  // Check reconnection
  if (!connected) {
    handleReconnect();
  }
  
  // Handle ping timeout
  if (connected && lastPingTime > 0) {
    unsigned long now = millis();
    // If no ping received for 2 minutes, connection may be lost
    if (now - lastPingTime > 120000) {
      Serial.println("[xiaozhi-mcp] Ping timeout, resetting connection");
      disconnect();
    }
  }
}

bool WebSocketMCP::isConnected() {
  return connected;
}

void WebSocketMCP::disconnect() {
  if (connected) {
    webSocket.disconnect();
    connected = false;
    lastPingTime = 0;
  }
}

void WebSocketMCP::handleReconnect() {
  // WebSocket library already has auto reconnect;
  // this mainly logs reconnect attempts and notifications
  unsigned long now = millis();
  if (!connected && (now - lastReconnectAttempt > currentBackoff || lastReconnectAttempt == 0)) {
    reconnectAttempt++;
    lastReconnectAttempt = now;
    
    // Calculate next backoff time (exponential backoff)
    currentBackoff = min(currentBackoff * 2, MAX_BACKOFF);
    
    Serial.println("[xiaozhi-mcp] Attempting reconnect (try: " + String(reconnectAttempt) + 
        ", next wait: " + String(currentBackoff / 1000.0, 2) + "s)");
  }
}

void WebSocketMCP::resetReconnectParams() {
  reconnectAttempt = 0;
  currentBackoff = INITIAL_BACKOFF;
  lastReconnectAttempt = 0;
}

// Handle JSON-RPC message
void WebSocketMCP::handleJsonRpcMessage(const String &message) {
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, message);
  
  if (error) {
    Serial.println("[xiaozhi-mcp] Failed to parse JSON: " + String(error.c_str()));
    return;
  }
  
  // Check if ping request
  if (doc.containsKey("method") && doc["method"] == "ping") {
    // Record last ping time
    lastPingTime = millis();
    
    // Build pong response
    String id = doc["id"].as<String>();
    Serial.println("[xiaozhi-mcp] Received ping: " + id);

    String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":{}}";
    sendMessage(response);

    Serial.println("[xiaozhi-mcp] Responded to ping: " + id);
  }
  // Handle initialize request
  else if (doc.containsKey("method") && doc["method"] == "initialize") {
    String id = doc["id"].as<String>();
    
    String serverName = "ESP-HA"; 

    // Send initialize response
    String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + 
      ",\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"experimental\":{},\"prompts\":{\"listChanged\":false},\"resources\":{\"subscribe\":false,\"listChanged\":false},\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"" + serverName + "\",\"version\":\"1.0.0\"}}}";
    
    sendMessage(response);
    Serial.println("[xiaozhi-mcp] Responded to initialize request");
    
    // Send initialized notification
    sendMessage("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
  }
  // Handle tools/list request
  else if (doc.containsKey("method") && doc["method"] == "tools/list") {
    String id = doc["id"].as<String>();
    
    // Build tools list
    String response = "{\"jsonrpc\":\"2.0\",\"id\":" + id + 
      ",\"result\":{\"tools\":[";
    
    // Generate tool info
    if (_tools.size() > 0) {
      for (size_t i = 0; i < _tools.size(); i++) {
        if (i > 0) {
          response += ",";
        }
        response += "{\"name\":\"" + _tools[i].name + "\",";
        response += "\"description\":\"" + _tools[i].description + "\",";
        response += "\"inputSchema\":" + _tools[i].inputSchema + "}";
      }
    }
    
    response += "]}}";
    
    sendMessage(response);
    Serial.println("[xiaozhi-mcp] Responded to tools/list request, total: " + String(_tools.size()));
  }
  // Handle tools/call request
  else if (doc.containsKey("method") && doc["method"] == "tools/call") {
    int id = doc["id"].as<int>();
    String toolName = doc["params"]["name"].as<String>();
    JsonObject arguments = doc["params"]["arguments"].as<JsonObject>();
    
    Serial.println("[xiaozhi-mcp] Tool call request: " + toolName);
    
    // Find tool
    bool toolFound = false;
    ToolResponse toolResponse;
    
    for (size_t i = 0; i < _tools.size(); i++) {
      if (_tools[i].name == toolName) {
        toolFound = true;
        // Call tool callback
        if (_tools[i].callback) {
          String argumentsJson;
          serializeJson(arguments, argumentsJson);
          toolResponse = _tools[i].callback(argumentsJson);
        } else {
          toolResponse = ToolResponse("{\"error\":\"Tool callback not registered\"}", true);
        }
        break;
      }
    }
    
    if (!toolFound) {
      toolResponse = ToolResponse("{\"error\":\"Tool not found: " + toolName + "\"}", true);
    }
    
    // Build JSON response
    DynamicJsonDocument responseDoc(2048);
    responseDoc["jsonrpc"] = "2.0";
    responseDoc["id"] = id;
    
    JsonObject result = responseDoc.createNestedObject("result");
    
    JsonArray content = result.createNestedArray("content");
    for (const auto& item : toolResponse.content) {
      JsonObject contentItem = content.createNestedObject();
      contentItem["type"] = item.type;
      contentItem["text"] = item.text;
    }

    result["isError"] = toolResponse.isError;
    
    String response;
    serializeJson(responseDoc, response);
    
    sendMessage(response);
    Serial.println("[xiaozhi-mcp] Tool call finished: " + toolName + (toolResponse.isError ? " (error)" : ""));
  }
}

// Escape special characters in JSON string
String WebSocketMCP::escapeJsonString(const String &input) {
  String result = "";
  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '\"' || c == '\\' || c == '/' || 
        c == '\b' || c == '\f' || c == '\n' || 
        c == '\r' || c == '\t') {
      if (c == '\"') result += "\\\"";
      else if (c == '\\') result += "\\\\";
      else if (c == '/') result += "\\/";
      else if (c == '\b') result += "\\b";
      else if (c == '\f') result += "\\f";
      else if (c == '\n') result += "\\n";
      else if (c == '\r') result += "\\r";
      else if (c == '\t') result += "\\t";
    } else {
      result += c;
    }
  }
  return result;
}

// Register tool (with callback)
bool WebSocketMCP::registerTool(const String &name, const String &description, 
                              const String &inputSchema, ToolCallback callback) {
  // Check if tool already exists
  for (size_t i = 0; i < _tools.size(); i++) {
    if (_tools[i].name == name) {
      // If tool exists, update callback
      _tools[i].callback = callback;
      Serial.println("[xiaozhi-mcp] Updated tool callback: " + name);
      return true;
    }
  }
  
  // Create new tool and add to list
  Tool newTool;
  newTool.name = name;
  newTool.description = description;
  newTool.inputSchema = inputSchema;
  newTool.callback = callback;
  
  _tools.push_back(newTool);
  Serial.println("[xiaozhi-mcp] Tool registered: " + name);
  return true;
}

// Simplified tool register method
bool WebSocketMCP::registerSimpleTool(const String &name, const String &description, 
                                    const String &paramName, const String &paramDesc, 
                                    const String &paramType, ToolCallback callback) {
  // Build simple input schema
  String inputSchema = "{\"type\":\"object\",\"properties\":{\"" + 
                      paramName + "\":{\"type\":\"" + paramType + 
                      "\",\"description\":\"" + paramDesc + 
                      "\"}},\"required\":[\"" + paramName + "\"]}";
                      
  return registerTool(name, description, inputSchema, callback);
}

// Unregister tool
bool WebSocketMCP::unregisterTool(const String &name) {
  for (size_t i = 0; i < _tools.size(); i++) {
    if (_tools[i].name == name) {
      _tools.erase(_tools.begin() + i);
      Serial.println("[xiaozhi-mcp] Tool removed: " + name);
      return true;
    }
  }
  Serial.println("[xiaozhi-mcp] Tool " + name + " not found, cannot remove");
  return false;
}

// Get number of tools
size_t WebSocketMCP::getToolCount() {
  return _tools.size();
}

// Clear all tools
void WebSocketMCP::clearTools() {
  _tools.clear();
  Serial.println("[WebSocketMCP] All tools cleared");
}

// Format JSON string, each key-value pair on one line
String WebSocketMCP::formatJsonString(const String &jsonStr) {
  // 1. Handle empty string or invalid JSON
  if (jsonStr.length() == 0) {
    return "{}";
  }

  // 2. Try to parse to ensure valid JSON
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, jsonStr);
  
  if (error) {
    // If parsing fails, return original string
    return jsonStr;
  }
  
  // 3. Initialize result
  String result = "{\n";
  
  // 4. Get all keys
  JsonObject obj = doc.as<JsonObject>();
  bool firstItem = true;
  
  // 5. Iterate through keys
  for (JsonPair p : obj) {
    if (!firstItem) {
      result += ",\n";
    }
    firstItem = false;
    
    result += "  \"" + String(p.key().c_str()) + "\": ";
    
    if (p.value().is<JsonObject>() || p.value().is<JsonArray>()) {
      String nestedJson;
      serializeJson(p.value(), nestedJson);
      result += nestedJson;
    } else if (p.value().is<const char*>() || p.value().is<String>()) {
      result += "\"" + String(p.value().as<const char*>()) + "\"";
    } else {
      String valueStr;
      serializeJson(p.value(), valueStr);
      result += valueStr;
    }
  }
  
  // 6. Close JSON
  result += "\n}";
  
  return result;
}
