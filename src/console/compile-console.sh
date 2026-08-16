#!/bin/bash
# Copyright (c) 2020 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.

# Console 独立仓库按指定版本构建；仅在 clone 和 make 全部成功后复制产物并清理临时目录。
git clone -b $1 https://github.com/vesoft-inc/nebula-console.git && \
pushd nebula-console && \
make && cp nebula-console $2/ && \
popd && \
rm -rf nebula-console
