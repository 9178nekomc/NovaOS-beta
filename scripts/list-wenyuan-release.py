#!/usr/bin/env python3
import json
import urllib.request

url = "https://api.github.com/repos/takushun-wu/WenYuanFonts/releases/latest"
req = urllib.request.Request(url, headers={"User-Agent": "curl"})
data = json.load(urllib.request.urlopen(req, timeout=30))
print("tag:", data["tag_name"])
for a in data.get("assets", []):
    print(a["name"], a["size"], a["browser_download_url"])
