source param.sh

# Configuration
sudo ~/NetChannel/scripts/run_np.sh $iface $1

# Run the server program
sudo taskset -c 0 ~/NetChannel/util/iouring_bench_nc server $server_ip 9095