#!/usr/bin/env python3
"""Shares a sketch with a hydra gallery server, with the requests of gallery.js in the web editor.

1. POST <server>/sketch?code=<base64>&parent=<sketch id>  answers the new sketch id.
2. POST <server>/image?url=<sketch URL>&sketch_id=<id>&name=<name>  with the screenshot as the multipart
   field "previewImage".
Prints the sketch URL. The code is base64 of encodeURIComponent(code), as encodeBase64() in gallery.js.

usage: gallery_share.py SERVER CODE_FILE IMAGE_FILE NAME [PARENT_SKETCH_ID]
"""
import base64
import sys
import urllib.parse
import urllib.request
import uuid

EDITOR_URL = "https://hydra.ojack.xyz/"
TIMEOUT_SECONDS = 30
# The characters that encodeURIComponent() of JavaScript leaves as they are.
URI_COMPONENT_SAFE = "-_.!~*'()"


def encode_uri_component(text):
    return urllib.parse.quote(text, safe=URI_COMPONENT_SAFE)


def post(url, body=b"", content_type="application/octet-stream"):
    request = urllib.request.Request(url, data=body, method="POST", headers={"Content-Type": content_type})
    with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as response:
        return response.read().decode()


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    server, code_file, image_file, name = sys.argv[1:5]
    parent = sys.argv[5] if len(sys.argv) > 5 and sys.argv[5] else None
    code = open(code_file, encoding="utf-8").read()
    code_base64 = base64.b64encode(encode_uri_component(code).encode()).decode()

    # superagent writes a null parent as the bare key "parent".
    query = urllib.parse.urlencode({"code": code_base64}) + ("&" + urllib.parse.urlencode({"parent": parent}) if parent else "&parent")
    sketch_id = post(f"{server}/sketch?{query}").strip()
    sketch_url = EDITOR_URL + "?" + urllib.parse.urlencode({"sketch_id": sketch_id, "code": code_base64})

    boundary = uuid.uuid4().hex
    image = open(image_file, "rb").read()
    body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"previewImage\"; filename=\"blob\"\r\n"
            f"Content-Type: image/png\r\n\r\n").encode() + image + f"\r\n--{boundary}--\r\n".encode()
    image_query = urllib.parse.urlencode({"url": sketch_url, "sketch_id": sketch_id, "name": name})
    post(f"{server}/image?{image_query}", body, f"multipart/form-data; boundary={boundary}")
    print(sketch_url)


if __name__ == "__main__":
    main()
