# RIST parameters

You're correct, libRIST v0.2.7 allows bufmin and bufmax configuration via rist_peer_config (seen in rist-common.c, ristreceiver.c, ristsender.c). The logs show bufmin=1000, bufmax=1000 (ms) as defaults. Override these in your rist_receiver_set_config call (e.g., set bufmin=50, bufmax=50) to reduce buffering delay. Ensure fifo_size is a power of 2 (e.g., 64ms) to avoid the "Desired fifo size must be a power of 2" error (snapclient.log: 19-19-15.410). Extend client hello timeout to >3 seconds as a fallback. Test and verify reduced latency.

## FIFO Size

`ret = rist_receiver_set_output_fifo_size(receiver_ctx_, 50); // 50ms FIFO - balanced approach`

## Polling

`int ret = rist_receiver_data_read2(receiver_ctx_, &data_block, 10); // 10ms timeout - matches FIFO stability`

## bufmin/bufmax and other (from stats)

librist/src/rist-common.c, line 288:

```cpp
		rist_log_priv(get_cctx(peer), RIST_LOG_INFO,
				"New peer with id #%"PRIu32" was configured with maxrate=%d/%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d congestion_control=%d min_retries=%d max_retries=%d\n",
				peer->adv_peer_id, peer->config.recovery_maxbitrate, peer->config.recovery_maxbitrate_return, peer->config.recovery_length_min, peer->config.recovery_length_max, peer->config.recovery_reorder_buffer,
				peer->config.recovery_rtt_min /RIST_CLOCK, peer->config.recovery_rtt_max /RIST_CLOCK, peer->config.congestion_control_mode, peer->config.min_retries, peer->config.max_retries);
```

* id: adv_peer_id
* maxrate: recovery_maxbitrate
* bufmin: recovery_length_min
* bufmax: recovery_length_max
* reorder: recovery_reorder_buffer
* rttmin: recovery_rtt_min
* rttmax: recovery_rtt_max
* congestion_control: congestion_control_mode
* min_retries: min_retries
* max_retries: max_retries

librist/tools/ristreceiver.c, line 826:

```cpp
		rist_log(&logging_settings, RIST_LOG_INFO, "Link configured with maxrate=%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d congestion_control=%d min_retries=%d max_retries=%d\n",
			peer_config->recovery_maxbitrate, peer_config->recovery_length_min, peer_config->recovery_length_max,
			peer_config->recovery_reorder_buffer, peer_config->recovery_rtt_min,peer_config->recovery_rtt_max,
			peer_config->congestion_control_mode, peer_config->min_retries, peer_config->max_retries);
```

* maxrate: recovery_maxbitrate
* bufmin: recovery_length_min
* bufmax: recovery_length_max
* reorder: recovery_reorder_buffer
* rttmin: recovery_rtt_min
* rttmax: recovery_rtt_max
* congestion_control: congestion_control_mode
* min_retries: min_retries
* max_retries: max_retries

librist/tools/ristsender.c, line 514:

```cpp
	rist_log(&logging_settings, RIST_LOG_INFO, "Link configured with maxrate=%d bufmin=%d bufmax=%d reorder=%d rttmin=%d rttmax=%d congestion_control=%d min_retries=%d max_retries=%d\n",
		peer_config_link->recovery_maxbitrate, peer_config_link->recovery_length_min, peer_config_link->recovery_length_max,
		peer_config_link->recovery_reorder_buffer, peer_config_link->recovery_rtt_min, peer_config_link->recovery_rtt_max,
		peer_config_link->congestion_control_mode, peer_config_link->min_retries, peer_config_link->max_retries);
```

* maxrate: recovery_maxbitrate
* bufmin: recovery_length_min
* bufmax: recovery_length_max
* reorder: recovery_reorder_buffer
* rttmin: recovery_rtt_min
* rttmax: recovery_rtt_max
* congestion_control: congestion_control_mode
* min_retries: min_retries
* max_retries: max_retries  

