connection_state_machine
	connection_handle_read_state
		connection_handle_read
			connection_handle_read_ssl
			connection_set_state

network_server_handle_fdevent
	connection_accept
		connection_handle_fdevent
			connection_set_state
			connection_handle_write
				network_write_chunkqueue