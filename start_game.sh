#!/bin/bash
make
sudo insmod keydance.ko
echo 1 > /proc/keydance-start
watch cat /proc/keydance-result
sudo rmmod keydance
