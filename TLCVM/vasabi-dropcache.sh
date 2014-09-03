cat /proc/meminfo | grep -iE "^(cached|dirty)"
sync; echo 3 | sudo tee /proc/sys/vm/drop_caches
cat /proc/meminfo | grep -iE "^(cached|dirty)"
