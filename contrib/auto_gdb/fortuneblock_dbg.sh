#!/bin/bash
# use testnet settings,  if you need mainnet,  use ~/.fortuneblockcore/fortuneblockd.pid file instead
fortuneblock_pid=$(<~/.fortuneblockcore/testnet3/fortuneblockd.pid)
sudo gdb -batch -ex "source debug.gdb" fortuneblockd ${fortuneblock_pid}
