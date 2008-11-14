require File.dirname(__FILE__) + '/maxconn_test'
backends = []
2.times { backends << MaxconnTest::DelayBackend.new(0.9) }
test_nginx(backends,
  :req_per_backend => 10,
  :max_connections => 1, # per backend, per worker
  :worker_processes => 1,
  :use_ssl => true
) do |nginx|
  out = %x{httperf --ssl --num-conns 20 --hog --timeout 10 --rate 100 --port #{nginx.port} --uri / }
  assert $?.exitstatus == 0
  results = httperf_parse_output(out)
  assert_equal 20, results["2xx"]
end
total_received = 0
backends.each do |b|
  assert_in_delta(10, b.experienced_requests, 2, 
    "backend #{b.port} is not balanced")

  assert(b.experienced_max_connections <= 1, 
    "backend #{b.port} had too many connections")

  total_received += b.experienced_requests
end
assert_equal 20, total_received, "backends did not recieve all requests"