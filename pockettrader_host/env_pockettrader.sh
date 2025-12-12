#!/usr/bin/env bash
# Common env config for PocketTrader scenarios

# BBB IP seen from laptop; override by exporting BBB_IP before running scripts
export BBB_IP="${BBB_IP:-192.168.7.2}"

# Ports must match pockettrader_core.c
export EXA_FEED_PORT=6001
export EXB_FEED_PORT=6002
export TRADE_PORT=7000

# Order ports for exchange_sim
export EXA_ORDER_PORT=9101
export EXB_ORDER_PORT=9102

# Fill port for trade_bridge
export FILL_PORT=7100

# Base prices / vol
export EXA_BASE=90000
export EXB_BASE_NORMAL=90001
export EXB_BASE_BURST=90003

export VOL_LOW=0.5
