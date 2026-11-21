import logging

from esphome.components import switch
import esphome.config_validation as cv
import esphome.codegen as cg

from .. import fourheat_config_validation as fhcv
from .. import (
    fourheat_ns,
    CONF_DATAPOINT,
    CONF_FOURHEAT_ID,
    CONF_PARSER,
    CONF_QUERY_DATAPOINT,
    FourHeat,
    get_parser_expression,
)

DEPENDENCIES = ["fourheat"]

FourHeatSwitch = fourheat_ns.class_("FourHeatSwitch", switch.Switch, cg.Component)

CONF_ON_DATAPOINT = "on_datapoint"
CONF_OFF_DATAPOINT = "off_datapoint"
CONF_ON_DATA = "on_data"
CONF_OFF_DATA = "off_data"
CONF_POWER_CONTROL = "power_control"
CONF_LEGACY_POWER_CONTROL = "legacy_power_control"
POWER_CONTROL_STANDARD = "standard"
POWER_CONTROL_GSM = "gsm"

_LOGGER = logging.getLogger(__name__)


def validate_power_control(config):
    mode = config.get(CONF_POWER_CONTROL)
    if CONF_LEGACY_POWER_CONTROL in config:
        alias_mode = POWER_CONTROL_GSM if config[CONF_LEGACY_POWER_CONTROL] else POWER_CONTROL_STANDARD
        if mode is not None and mode != alias_mode:
            raise cv.Invalid("legacy_power_control conflicts with power_control", [CONF_LEGACY_POWER_CONTROL])
        _LOGGER.warning("legacy_power_control is deprecated. Use power_control: %s", alias_mode)
        mode = alias_mode
    config[CONF_POWER_CONTROL] = mode or POWER_CONTROL_STANDARD

    if config[CONF_POWER_CONTROL] == POWER_CONTROL_GSM:
        if config.get(CONF_DATAPOINT, "J30001") != "J30001":
            raise cv.Invalid("GSM power control uses datapoint J30001", [CONF_DATAPOINT])
        if config.get(CONF_QUERY_DATAPOINT, "I30001") != "I30001":
            raise cv.Invalid("GSM power control uses query I30001", [CONF_QUERY_DATAPOINT])
        config.setdefault(CONF_DATAPOINT, "J30001")
    elif CONF_DATAPOINT not in config:
        raise cv.Invalid("datapoint is required for standard power control", [CONF_DATAPOINT])
    return config


CONFIG_SCHEMA = cv.All(
    (
        switch.switch_schema(FourHeatSwitch)
        .extend(
            {
                cv.GenerateID(CONF_FOURHEAT_ID): cv.use_id(FourHeat),
                cv.Optional(CONF_DATAPOINT): fhcv.datapoint,
                cv.Optional(CONF_QUERY_DATAPOINT): fhcv.datapoint,

                cv.Optional(CONF_PARSER): cv.returning_lambda,

                cv.Optional(CONF_ON_DATAPOINT): fhcv.datapoint,
                cv.Optional(CONF_OFF_DATAPOINT): fhcv.datapoint,
                cv.Optional(CONF_ON_DATA): fhcv.data,
                cv.Optional(CONF_OFF_DATA): fhcv.data,
                cv.Optional(CONF_LEGACY_POWER_CONTROL): cv.boolean,
                cv.Optional(CONF_POWER_CONTROL): cv.one_of(POWER_CONTROL_STANDARD, POWER_CONTROL_GSM, lower=True),
            }
        )
        .extend(cv.COMPONENT_SCHEMA)
    ),
    validate_power_control,
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_FOURHEAT_ID])
    cg.add(var.set_fourheat_parent(parent))
    cg.add(var.set_datapoint_id(config[CONF_DATAPOINT]))

    if config[CONF_POWER_CONTROL] == POWER_CONTROL_GSM:
        cg.add(var.enable_gsm_power_control())
        return

    if CONF_QUERY_DATAPOINT in config:
        cg.add(var.set_query_datapoint_id(config[CONF_QUERY_DATAPOINT]))

    if CONF_PARSER in config:
        lambda_ = await get_parser_expression(config[CONF_PARSER], bool)
        cg.add(var.set_parser(lambda_))

    if CONF_ON_DATAPOINT in config:
        cg.add(var.set_on_datapoint_id(config[CONF_ON_DATAPOINT]))
    if CONF_OFF_DATAPOINT in config:
        cg.add(var.set_off_datapoint_id(config[CONF_OFF_DATAPOINT]))
    if CONF_ON_DATA in config:
        cg.add(var.set_on_data(config[CONF_ON_DATA]))
    if CONF_OFF_DATA in config:
        cg.add(var.set_off_data(config[CONF_OFF_DATA]))
