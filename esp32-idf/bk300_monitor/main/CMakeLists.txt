# Регистрируем компонент main. Добавляем зависимости от bt и nvs_flash.
idf_component_register(
    SRCS
        "main.c"
        "bk300_protocol.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        nvs_flash
)
