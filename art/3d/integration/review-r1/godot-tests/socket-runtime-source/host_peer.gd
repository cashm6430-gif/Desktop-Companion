extends RefCounted

# Authenticated localhost NDJSON transport. Tokens are never recorded in reports.
signal message_received(message: Dictionary)
signal connection_changed(connected: bool)
signal protocol_problem(reason: String)

const MAX_LINE_BYTES := 16384
const CONNECT_TIMEOUT_USEC := 5000000
var port := 0
var token := ""
var capabilities: Array[String] = []
var capability_details: Dictionary = {}
var connected := false
var session_id := ""
var connection_id := ""
var _socket: StreamPeerTCP
var _buffer := PackedByteArray()
var _out_seq := 0
var _in_seq := 0
var _next_connect_usec := 0
var _connect_started_usec := 0
var _hello_sent := false
var _closed := false

func configure(host_port: int, host_token: String, names: Array[String], details: Dictionary) -> void:
	port = host_port
	token = host_token
	capabilities = names
	capability_details = details

func poll() -> void:
	if _closed:
		return
	var now := Time.get_ticks_usec()
	if _socket == null:
		if now < _next_connect_usec:
			return
		_socket = StreamPeerTCP.new()
		_connect_started_usec = now
		if _socket.connect_to_host("127.0.0.1", port) != OK:
			_drop("connect_failed")
			return
	_socket.poll()
	var status := _socket.get_status()
	if status == StreamPeerTCP.STATUS_ERROR or (status == StreamPeerTCP.STATUS_NONE and _hello_sent):
		_drop("disconnected")
		return
	if not connected and now - _connect_started_usec > CONNECT_TIMEOUT_USEC:
		_drop("handshake_timeout")
		return
	if status != StreamPeerTCP.STATUS_CONNECTED:
		return
	if not _hello_sent:
		_hello_sent = true
		_write({"v": 1, "type": "hello", "token": token})
	var available := _socket.get_available_bytes()
	if available > 0:
		var result := _socket.get_data(mini(available, MAX_LINE_BYTES + 1))
		if result[0] != OK:
			_drop("read_failed")
			return
		_buffer.append_array(result[1])
	while not _buffer.is_empty():
		var newline := _buffer.find(10)
		if newline < 0:
			if _buffer.size() > MAX_LINE_BYTES:
				_drop("oversized_line")
			return
		if newline > MAX_LINE_BYTES:
			_drop("oversized_line")
			return
		var line := _buffer.slice(0, newline).get_string_from_utf8()
		_buffer = _buffer.slice(newline + 1)
		var parsed := JSON.new()
		if parsed.parse(line) != OK or not parsed.data is Dictionary:
			_drop("invalid_json")
			return
		_receive(parsed.data)
		if _socket == null:
			return

func send(type_name: String, fields: Dictionary = {}) -> bool:
	if not connected:
		return false
	_out_seq += 1
	var envelope := fields.duplicate(true)
	envelope.merge({"v": 1, "type": type_name, "session_id": session_id,
		"connection_id": connection_id, "seq": _out_seq}, true)
	return _write(envelope)

func close() -> void:
	_closed = true
	if _socket != null:
		_socket.disconnect_from_host()
	_socket = null
	connected = false

func _write(message: Dictionary) -> bool:
	if _socket == null:
		return false
	var data := (JSON.stringify(message) + "\n").to_utf8_buffer()
	if data.size() > MAX_LINE_BYTES + 1:
		protocol_problem.emit("outbound_line_too_large")
		return false
	if _socket.put_data(data) != OK:
		_drop("write_failed")
		return false
	return true

func _receive(message: Dictionary) -> void:
	if message.get("v") != 1:
		_drop("wrong_version")
		return
	if not connected:
		if message.get("type") != "welcome" or message.get("seq") != 1:
			_drop("expected_welcome")
			return
		session_id = String(message.get("session_id", ""))
		connection_id = String(message.get("connection_id", ""))
		if session_id.is_empty() or connection_id.is_empty():
			_drop("missing_connection_identity")
			return
		_in_seq = 1
		_out_seq = 0
		connected = true
		if send("ready", {"capabilities": capabilities, "capability_details": capability_details}):
			connection_changed.emit(true)
		return
	if message.get("session_id") != session_id or message.get("connection_id") != connection_id:
		protocol_problem.emit("stale_connection_ignored")
		return
	var incoming_seq: Variant = message.get("seq", 0)
	if not (incoming_seq is int or incoming_seq is float) or float(incoming_seq) != floorf(float(incoming_seq)) or int(incoming_seq) <= _in_seq:
		protocol_problem.emit("stale_sequence_ignored")
		return
	_in_seq = int(incoming_seq)
	message_received.emit(message)

func _drop(reason: String) -> void:
	var was_connected := connected
	if _socket != null:
		_socket.disconnect_from_host()
	_socket = null
	_buffer.clear()
	_hello_sent = false
	connected = false
	session_id = ""
	connection_id = ""
	_in_seq = 0
	_out_seq = 0
	_next_connect_usec = Time.get_ticks_usec() + 1000000
	protocol_problem.emit(reason)
	if was_connected:
		connection_changed.emit(false)
