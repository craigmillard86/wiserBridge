idf_component_register(
    SRCS "sniffer_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ieee802154 esp_driver_usb_serial_jtag esp_driver_gpio vfs esp_timer
)
