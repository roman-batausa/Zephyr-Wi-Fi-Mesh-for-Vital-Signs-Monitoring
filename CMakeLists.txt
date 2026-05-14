cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(wifi_mesh_sensor_mqtt)

target_sources(app PRIVATE
    src/main.c
    src/zephyr_mesh.c
    src/mqtt_publisher.c
)
