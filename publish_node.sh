#!/bin/bash

set -e

./build_node.sh

rm -r ./dist || true
mkdir dist

cp ./build/Release/wcjs-gs.node ./README.md ./dist/

cat <<EOF > ./dist/index.js
module.exports = require("./wcjs-gs.node");
EOF

cat <<EOF > ./dist/package.json
{
  "name": "wcjs-gs-prebuilt",
  "description": "WebChimera.js GStreamer edition",
  "version": "`node -p "require('./package.json').version"`",
  "license": "LGPL-2.1",
  "author": "Sergey Radionov <rsatom@gmail.com>",
  "keywords": [
    "wcjs",
    "gstreamer"
  ],
  "repository": {
    "type": "git",
    "url": "https://github.com/RSATom/wcjs-gs"
  }
}
EOF

cd dist
npm publish

cd -
