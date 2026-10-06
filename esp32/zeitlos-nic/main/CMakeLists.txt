idf_component_register(
	SRCS "main.c" "znic.c" "gateway.c" "selftest.c" "tftpd.c" "screend.c"
	INCLUDE_DIRS "."
	EMBED_TXTFILES "../web/index.html")

# /ws is a plain GET until the origin check passes, and the handshake
# is finished here. ESP-IDF 5.4 sends the 101 for a websocket URI
# before the handler runs. The handshake function is private to that IDF.
idf_component_get_property(httpd_dir esp_http_server COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PRIVATE
	"${httpd_dir}/src" "${httpd_dir}/src/port/esp32")
