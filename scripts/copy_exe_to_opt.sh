#!/bin/env bash

sudo rm -f /opt/nui-sftp/bin/nui-sftp
sudo cp -f ./build/clang_release/bin/nui-sftp /opt/nui-sftp/bin/nui-sftp

sudo rm -rf /opt/nui-sftp/frontend
sudo mkdir -p /opt/nui-sftp/frontend
sudo cp -rf ./build/clang_release/module_nui-sftp/bin/. /opt/nui-sftp/frontend