import json
import urllib.request
import winreg


def token():
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                        r"Software\JavaSoft\Prefs\io\github\psd2live\agent") as key:
        value, _ = winreg.QueryValueEx(key, "agent_mcp_bearer_token")
    result = []
    i = 0
    while i < len(value):
        if value[i] == "/" and i + 1 < len(value):
            i += 1
            result.append("/" if value[i] == "/" else value[i].upper())
        else:
            result.append(value[i])
        i += 1
    return "".join(result)


def call(method, params, ident, session=None):
    headers = {
        "Accept": "application/json, text/event-stream",
        "Authorization": "Bearer " + token(),
        "Content-Type": "application/json",
    }
    if session:
        headers["Mcp-Session-Id"] = session
        headers["MCP-Protocol-Version"] = "2025-06-18"
    data = json.dumps({"jsonrpc": "2.0", "id": ident,
                       "method": method, "params": params}).encode()
    request = urllib.request.Request("http://127.0.0.1:23871/mcp", data,
                                     headers, method="POST")
    with urllib.request.urlopen(request, timeout=180) as response:
        body = response.read().decode()
        if body.startswith("event:"):
            body = next(line[5:].strip() for line in body.splitlines()
                        if line.startswith("data:"))
        return json.loads(body), response.headers.get("Mcp-Session-Id", session)


def initialize():
    return call("initialize", {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "desktop-companion-build", "version": "0.1"},
    }, 1)


if __name__ == "__main__":
    result, session = initialize()
    print("initialize", result.get("result", {}).get("serverInfo"), bool(session))
    result, _ = call("tools/list", {}, 2, session)
    for tool in result.get("result", {}).get("tools", []):
        if tool["name"] in {"asset", "view", "preview"}:
            print(tool["name"], json.dumps(tool.get("inputSchema", {}), ensure_ascii=False))
    result, _ = call("tools/call", {"name": "inspect", "arguments": {"scope": "project"}}, 3, session)
    print("project", json.dumps(result, ensure_ascii=False)[:2000])
