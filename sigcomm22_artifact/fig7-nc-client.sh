source param.sh

# Configuration
sudo ~/NetChannel/scripts/run_np.sh $iface

# Run the client program
echo ""
echo "### Run redis_populate ###"
~/redis/redis_populate-nc
echo ""

for qd in 1 2 4 8 16 32 64; do
	echo "### Queue depth = $qd ###"
	taskset -c 0-31:4 ~/redis/redis_async-nc 192.168.10.117 6379 8 0.75 1 $qd
	echo ""
done
