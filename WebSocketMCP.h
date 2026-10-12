#ifndef WEBSOCKET_MCP_H
#define WEBSOCKET_MCP_H

#include <Arduino.h>
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif
#include <WebSocketsClient.h>
#include <ArduinoJson.h>  // Need to include this library to parse JSON
#include <vector>
#include <functional>

/**
 * WebSocketMCP class
 * Encapsulates the WebSocket connection and communication with the MCP server
 */
class WebSocketMCP {
public:
  // Define tool response content structure
  // Sending to WebSocket: {"jsonrpc":"2.0","id":48,"result":{"content":[{"type":"text","text":"{\n  \"success\": true,\n  \"result\": 2\n}"}],"isError":false}}
  struct ToolContentItem {
    String type;  // Content type, e.g. "text"
    String text;  // Text content
  };
  
  // Define tool response structure
  struct ToolResponse {
    std::vector<ToolContentItem> content;  // Response content array
    bool isError;                          // Whether this is an error response
    
    // Constructor: create response from a single text content
    ToolResponse(const String& textContent, bool error = false) {
      ToolContentItem item;
      item.type = "text";
      
      // Try to detect if this is JSON and format it (by checking for leading { and trailing })
      String trimmedText = textContent;
      trimmedText.trim();
      
      if (trimmedText.startsWith("{") && trimmedText.endsWith("}")) {
        // Use WebSocketMCP::formatJsonString method to format
        // Since this is inside a static method, create a temporary instance to call non-static method
        WebSocketMCP* instance = WebSocketMCP::instance;
        if (instance) {
          item.text = instance->formatJsonString(textContent);
        } else {
          item.text = textContent;
        }
      } else {
        item.text = textContent;
      }
      
      content.push_back(item);
      isError = error;
    }

    // New constructor: handle bool and String parameters
    ToolResponse(bool error, const String& message) {
      ToolContentItem item;
      item.type = "text";
      item.text = message;
      content.push_back(item);
      isError = error;
    }
    
    // Default constructor
    ToolResponse() : isError(false) {}
    
    // Create response from JSON object (convenience method)
    static ToolResponse fromJson(const JsonObject& json, bool error = false) {
      String jsonStr;
      serializeJson(json, jsonStr);
      return ToolResponse(jsonStr, error);
    }
  };

  // Helper class for parameter handling
  class ToolParams {
  public:
    ToolParams(const String& json) {
      DeserializationError error = deserializeJson(doc, json);
      valid = !error;
    }

    // NEW: Static factory method from JsonVariantConst
    // Serializes the variant and re-parses it to create a ToolParams
    static ToolParams fromVariant(const JsonVariantConst& variant) {
      String variantJson;
      serializeJson(variant, variantJson);
      return ToolParams(variantJson);
    }
    
    template<typename T>
    T get(const String& key, T defaultValue) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return defaultValue;
      }
      return doc.as<JsonObjectConst>()[key].as<T>();
    }
    
    JsonVariantConst getJsonValue(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return JsonVariantConst();
      }
      return doc.as<JsonObjectConst>()[key];
    }
    
    JsonArrayConst getJsonArray(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key) || !doc.as<JsonObjectConst>()[key].is<JsonArray>()) {
        return JsonArrayConst();
      }
      return doc.as<JsonObjectConst>()[key].as<JsonArrayConst>();
    }
    
    bool isArray(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key)) {
        return false;
      }
      return doc.as<JsonObjectConst>()[key].is<JsonArray>();
    }
    
    size_t getArraySize(const String& key) const {
      if (!valid || !doc.is<JsonObject>() || !doc.as<JsonObjectConst>().containsKey(key) || !doc.as<JsonObjectConst>()[key].is<JsonArray>()) {
        return 0;
      }
      return doc.as<JsonObjectConst>()[key].as<JsonArrayConst>().size();
    }

    // NEW: Check if root is a JSON object
    bool isJsonObject() const {
      return valid && doc.is<JsonObject>();
    }

    // NEW: Check if root is a JSON array
    bool isJsonArray() const {
      return valid && doc.is<JsonArray>();
    }

    // NEW: Get root as JsonObjectConst
    JsonObjectConst getAsJsonObject() const {
      if (isJsonObject()) {
        return doc.as<JsonObjectConst>();
      }
      return JsonObjectConst();
    }

    // NEW: Get root as JsonArrayConst
    JsonArrayConst getAsJsonArray() const {
      if (isJsonArray()) {
        return doc.as<JsonArrayConst>();
      }
      return JsonArrayConst();
    }
    
    bool contains(const String& key) const {
      return valid && doc.is<JsonObject>() && doc.as<JsonObjectConst>().containsKey(key);
    }
    
    String getDebugJson() const {
      String result;
      if (valid) {
        serializeJson(doc, result);
      } else {
        result = "{\"error\":\"Invalid JSON document in ToolParams\"}";
      }
      return result;
    }
    
    bool isValid() const {
      return valid;
    }
    
  private:
    DynamicJsonDocument doc{2048}; // Adjust size as needed
    bool valid = false;
  };

  // Redefine tool callback function type — receives JSON string and returns ToolResponse
  typedef std::function<ToolResponse(const String&)> ToolCallback;

  // Callback type definitions
  typedef void (*OutputCallback)(const String&);
  typedef void (*ErrorCallback)(const String&);
  typedef void (*ConnectionCallback)(bool);

  WebSocketMCP();

  /**
   * Initialize WebSocket connection
   * @param mcpEndpoint WebSocket server address (ws://host:port/path)
   * @param outputCb Output callback (equivalent to stdout)
   * @param errorCb Error callback (equivalent to stderr)
   * @param connCb Connection state callback
   * @return Whether initialization succeeded
   * 
   * Note: Connection uses the following timeout settings:
   * - PING_INTERVAL: Ping heartbeat interval (default 45s)
   * - DISCONNECT_TIMEOUT: Disconnect timeout (default 60s)
   * - INITIAL_BACKOFF: Initial reconnect wait time (default 1s)
   * - MAX_BACKOFF: Maximum reconnect wait time (default 60s)
   */
  bool begin(const char *mcpEndpoint, ConnectionCallback connCb = nullptr);

  /**
   * Send message to WebSocket server (equivalent to stdin)
   * @param message Message to send
   * @return Whether sending succeeded
   */
  bool sendMessage(const String &message);

  /**
   * Handle WebSocket events and keep the connection alive  
   * Must be called frequently in the main loop
   */
  void loop();

  /**
   * Check whether connected to the server
   * @return Connection state
   */
  bool isConnected();

  /**
   * Disconnect from server
   */
  void disconnect();

  // Tool management API
  bool registerTool(const String &name, const String &description, const String &inputSchema, ToolCallback callback);
  bool registerSimpleTool(const String &name, const String &description, 
                         const String &paramName, const String &paramDesc, 
                         const String &paramType, ToolCallback callback);
  
  bool unregisterTool(const String &name);
  size_t getToolCount();
  void clearTools();

private:
  WebSocketsClient webSocket;
  ConnectionCallback connectionCallback;

  bool connected;
  unsigned long lastReconnectAttempt;

  // Reconnect settings
  static const int INITIAL_BACKOFF = 1000;        // Initial wait time (ms)
  static const int MAX_BACKOFF = 60000;           // Maximum wait time (ms)
  static const int PING_INTERVAL = 10000;         // Ping interval (ms)
  static const int DISCONNECT_TIMEOUT = 60000;    // Disconnect timeout (ms)
  int currentBackoff;
  int reconnectAttempt;

  // WebSocket event handler
  static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
  static WebSocketMCP *instance;

  // Reconnect handling
  void handleReconnect();
  void resetReconnectParams();

  // Additional members
  unsigned long lastPingTime = 0;
  void handleJsonRpcMessage(const String &message);

  // Tool structure
  struct Tool {
    String name;           // Tool name
    String description;    // Tool description
    String inputSchema;    // Input schema (JSON format)
    ToolCallback callback; // Tool handler callback
  };

  // Tool list
  std::vector<Tool> _tools;

  // Helper methods
  String escapeJsonString(const String &input);
  
  // Format JSON string, each key-value pair on a new line
  String formatJsonString(const String &jsonStr);
};

#endif // WEBSOCKET_MCP_H
