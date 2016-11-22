#!/bin/bash
make
sudo insmod keydance.ko
echo 1 > /proc/keydance-start
watch -n 0.1 cat /proc/keydance-result
sudo rmmod keydance
