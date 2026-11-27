import datetime
from enum import IntEnum
import re
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import mgp

from mage.date.constants import Epoch
from mage.date.unit_conversion import to_int, to_timedelta


def validate_timezone(timezone: str) -> ZoneInfo:
    if not isinstance(timezone, str) or not timezone:
        raise Exception("Timezone doesn't exist. Check documentation to see available timezones.")
    try:
        return ZoneInfo(timezone)
    except (ZoneInfoNotFoundError, ValueError, KeyError) as err:
        raise Exception("Timezone doesn't exist. Check documentation to see available timezones.") from err


def localize_wall_datetime(naive_dt: datetime.datetime, zone: ZoneInfo) -> datetime.datetime:
    z0 = naive_dt.replace(tzinfo=zone, fold=0)
    z1 = naive_dt.replace(tzinfo=zone, fold=1)
    if z0.utcoffset() != z1.utcoffset() and z0.dst():
        return z1
    return z0


@mgp.read_proc
def parse(
    time: str,
    unit: str = "ms",
    format: str = "%Y-%m-%d %H:%M:%S",
    timezone: str = "UTC",
) -> mgp.Record(parsed=int):
    first_date = Epoch.UNIX_EPOCH.replace(tzinfo=datetime.timezone.utc)
    input_date = datetime.datetime.strptime(time, format)
    zone = validate_timezone(timezone)

    if input_date.tzinfo is None:
        input_date = localize_wall_datetime(input_date, zone)

    time_since = input_date - first_date

    if unit == "ms":
        parsed = time_since // datetime.timedelta(milliseconds=1)
    elif unit == "s":
        parsed = time_since // datetime.timedelta(seconds=1)
    elif unit == "m":
        parsed = time_since // datetime.timedelta(minutes=1)
    elif unit == "h":
        parsed = time_since // datetime.timedelta(hours=1)
    elif unit == "d":
        parsed = time_since // datetime.timedelta(days=1)
    else:
        raise Exception("Unit doesn't exist. Check documentation to see available units.")

    return mgp.Record(parsed=parsed)


@mgp.read_proc
def format(
    time: int,
    unit: str = "ms",
    format: str = "%Y-%m-%d %H:%M:%S %Z",
    timezone: str = "UTC",
) -> mgp.Record(formatted=str):
    first_date = Epoch.UNIX_EPOCH.replace(tzinfo=datetime.timezone.utc)

    if unit == "ms":
        new_date = first_date + datetime.timedelta(milliseconds=time)
    elif unit == "s":
        new_date = first_date + datetime.timedelta(seconds=time)
    elif unit == "m":
        new_date = first_date + datetime.timedelta(minutes=time)
    elif unit == "h":
        new_date = first_date + datetime.timedelta(hours=time)
    elif unit == "d":
        new_date = first_date + datetime.timedelta(days=time)
    else:
        raise Exception("Unit doesn't exist. Check documentation to see available units.")

    zone = validate_timezone(timezone)
    local_date = new_date.astimezone(zone)
    return mgp.Record(formatted=local_date.strftime(format))


@mgp.function
def add(
    time: int,
    unit: str,
    add_value: int,
    add_unit: str,
) -> int:
    return to_int(
        to_timedelta(time=time, unit=unit) + to_timedelta(time=add_value, unit=add_unit),
        unit=unit,
    )


class FormatLength(IntEnum):
    """Enum for various date/time format lengths to replace magic numbers"""

    BASIC_DATE = 8  # Length of 'YYYYMMDD'
    DATE = 10  # Length of 'YYYY-MM-DD'
    TIME = 8  # Length of 'HH:MM:SS'


class DateFormatUtil:
    """
    Utility class for converting between predefined ISO date formats.
    """

    ISO_DATE_FORMATS = {
        "basic_iso_date": "%Y%m%d",  # BASIC_ISO_DATE: '20111203'
        "iso_local_date": "%Y-%m-%d",  # ISO_LOCAL_DATE: '2011-12-03'
        "iso_offset_date": "%Y-%m-%d%z",  # ISO_OFFSET_DATE: '2011-12-03+01:00'
        "iso_date": "%Y-%m-%d",  # ISO_DATE: '2011-12-03' or '2011-12-03+01:00'
        "iso_local_time": "%H:%M:%S",  # ISO_LOCAL_TIME: '10:15:30'
        "iso_offset_time": "%H:%M:%S%z",  # ISO_OFFSET_TIME: '10:15:30+01:00'
        "iso_time": "%H:%M:%S",  # ISO_TIME: '10:15:30' or '10:15:30+01:00'
        "iso_local_date_time": "%Y-%m-%dT%H:%M:%S",  # ISO_LOCAL_DATE_TIME: '2011-12-03T10:15:30'
        "iso_offset_date_time": "%Y-%m-%dT%H:%M:%S%z",  # ISO_OFFSET_DATE_TIME: '2011-12-03T10:15:30+01:00'
        "iso_zoned_date_time": "iso_zoned_date_time",  # ISO_ZONED_DATE_TIME: '2011-12-03T10:15:30+01:00[Europe/Paris]'
        "iso_date_time": "iso_date_time",  # ISO_DATE_TIME: local, offset, or zoned ISO date-time
    }

    @staticmethod
    def get_format(format_str: str) -> str:
        format_lower = format_str.lower()
        if format_lower not in DateFormatUtil.ISO_DATE_FORMATS:
            raise ValueError(f"Unsupported date format: {format_str}")
        return DateFormatUtil.ISO_DATE_FORMATS[format_lower]


def strip_zone_bracket(temporal: str) -> str:
    if "[" not in temporal and "]" not in temporal:
        return temporal
    if not temporal.endswith("]") or temporal.count("[") != 1 or temporal.count("]") != 1:
        raise ValueError(f"Malformed zone suffix in '{temporal}'")
    prefix, zone_part = temporal[:-1].split("[", 1)
    if not zone_part:
        raise ValueError(f"Empty zone suffix in '{temporal}'")
    return prefix


def parse_iso_temporal(temporal: str, format_name: str) -> datetime.datetime:
    fmt = format_name.lower()
    DateFormatUtil.get_format(fmt)
    normalized = re.sub(r"(\.\d{6})\d*", r"\1", temporal)

    if fmt == "basic_iso_date":
        if len(temporal) != FormatLength.BASIC_DATE or not temporal.isdigit():
            raise ValueError(f"Invalid basic_iso_date: '{temporal}'")
        return datetime.datetime.strptime(temporal, "%Y%m%d")

    if fmt == "iso_local_date":
        if len(temporal) != FormatLength.DATE:
            raise ValueError(f"Invalid iso_local_date: '{temporal}'")
        parsed_date = datetime.date.fromisoformat(temporal)
        return datetime.datetime(parsed_date.year, parsed_date.month, parsed_date.day)

    if fmt == "iso_offset_date":
        if len(temporal) <= FormatLength.DATE or temporal[FormatLength.DATE] not in "+-Zz":
            raise ValueError(f"Invalid iso_offset_date: '{temporal}'")
        return datetime.datetime.strptime(temporal, "%Y-%m-%d%z")

    if fmt == "iso_date":
        if len(temporal) == FormatLength.DATE:
            parsed_date = datetime.date.fromisoformat(temporal)
            return datetime.datetime(parsed_date.year, parsed_date.month, parsed_date.day)
        if len(temporal) > FormatLength.DATE and temporal[FormatLength.DATE] in "+-Zz":
            return datetime.datetime.strptime(temporal, "%Y-%m-%d%z")
        raise ValueError(f"Invalid iso_date: '{temporal}'")

    if fmt in ("iso_local_time", "iso_offset_time", "iso_time"):
        if ":" not in normalized or "T" in normalized or "t" in normalized or " " in normalized:
            raise ValueError(f"Invalid {fmt}: '{temporal}'")
        parsed_time = datetime.time.fromisoformat(normalized)
        if fmt == "iso_local_time" and parsed_time.tzinfo is not None:
            raise ValueError(f"Unexpected offset in iso_local_time: '{temporal}'")
        if fmt == "iso_offset_time" and parsed_time.tzinfo is None:
            raise ValueError(f"Missing offset in iso_offset_time: '{temporal}'")
        return datetime.datetime(
            1900,
            1,
            1,
            parsed_time.hour,
            parsed_time.minute,
            parsed_time.second,
            parsed_time.microsecond,
            tzinfo=parsed_time.tzinfo,
        )

    if fmt == "iso_local_date_time":
        if ("T" not in normalized and "t" not in normalized) or "[" in normalized or "]" in normalized:
            raise ValueError(f"Invalid iso_local_date_time: '{temporal}'")
        dt = datetime.datetime.fromisoformat(normalized)
        if dt.tzinfo is not None:
            raise ValueError(f"Unexpected offset in iso_local_date_time: '{temporal}'")
        return dt

    if fmt == "iso_offset_date_time":
        if ("T" not in normalized and "t" not in normalized) or "[" in normalized or "]" in normalized:
            raise ValueError(f"Invalid iso_offset_date_time: '{temporal}'")
        dt = datetime.datetime.fromisoformat(normalized)
        if dt.tzinfo is None:
            raise ValueError(f"Missing offset in iso_offset_date_time: '{temporal}'")
        return dt

    if fmt == "iso_zoned_date_time":
        without_zone = re.sub(r"(\.\d{6})\d*", r"\1", strip_zone_bracket(temporal))
        if "T" not in without_zone and "t" not in without_zone:
            raise ValueError(f"Invalid iso_zoned_date_time: '{temporal}'")
        dt = datetime.datetime.fromisoformat(without_zone)
        if dt.tzinfo is None:
            raise ValueError(f"Missing offset in iso_zoned_date_time: '{temporal}'")
        return dt

    if fmt == "iso_date_time":
        has_zone = "[" in temporal
        without_zone = re.sub(r"(\.\d{6})\d*", r"\1", strip_zone_bracket(temporal))
        if "T" not in without_zone and "t" not in without_zone:
            raise ValueError(f"Invalid iso_date_time: '{temporal}'")
        dt = datetime.datetime.fromisoformat(without_zone)
        if has_zone and dt.tzinfo is None:
            raise ValueError(f"Zone suffix without offset in iso_date_time: '{temporal}'")
        return dt

    raise ValueError(f"Unsupported date format: {format_name}")


def format_iso_temporal(dt: datetime.datetime, target_format: str) -> str:
    fmt = target_format.lower()
    DateFormatUtil.get_format(fmt)

    if fmt in ("iso_offset_date", "iso_offset_time", "iso_offset_date_time") and dt.tzinfo is None:
        raise Exception("missing timezone")

    if fmt == "basic_iso_date":
        return dt.strftime("%Y%m%d")

    if fmt == "iso_local_date":
        return dt.date().isoformat()

    if fmt == "iso_offset_date":
        offset_str = dt.replace(hour=0, minute=0, second=0, microsecond=0).timetz().isoformat()[FormatLength.TIME :]
        return f"{dt.date().isoformat()}{offset_str}"

    if fmt == "iso_date":
        if dt.tzinfo is not None:
            offset_str = dt.replace(hour=0, minute=0, second=0, microsecond=0).timetz().isoformat()[FormatLength.TIME :]
            return f"{dt.date().isoformat()}{offset_str}"
        return dt.date().isoformat()

    if fmt == "iso_local_time":
        return datetime.time(dt.hour, dt.minute, dt.second, dt.microsecond).isoformat()

    if fmt == "iso_offset_time":
        return dt.timetz().isoformat()

    if fmt == "iso_time":
        if dt.tzinfo is not None:
            return dt.timetz().isoformat()
        return datetime.time(dt.hour, dt.minute, dt.second, dt.microsecond).isoformat()

    if fmt == "iso_local_date_time":
        return dt.replace(tzinfo=None).isoformat()

    if fmt == "iso_offset_date_time":
        return dt.isoformat()

    if fmt == "iso_zoned_date_time":
        aware_dt = dt if dt.tzinfo is not None else dt.replace(tzinfo=ZoneInfo("UTC"))
        return aware_dt.isoformat()

    if fmt == "iso_date_time":
        return dt.isoformat()

    raise ValueError(f"Unsupported date format: {target_format}")


@mgp.function
def convert_format(temporal: mgp.Nullable[str], current_format: str, convert_to: str) -> mgp.Nullable[str]:
    """
    Converts between specified ISO date formats.
    Supports zoned to offset conversion by removing zone part in '[]'.
    Offset to zoned returns the same string.
    Throws if parsing fails.

    Args:
        temporal: The datetime string to convert
        current_format: The current format of the datetime string
        convert_to: The target format to convert to

    Returns:
        output: The converted datetime string or None if input is None or empty
    """
    if temporal is None or temporal.strip() == "":
        return None

    try:
        dt = parse_iso_temporal(temporal, current_format)
        return format_iso_temporal(dt, convert_to)
    except Exception as e:
        raise Exception(f"Error converting '{temporal}' from '{current_format}' to '{convert_to}': {e}")


@mgp.read_proc
def get_date_formats(context: mgp.ProcCtx) -> mgp.Record(formats=mgp.List[str]):
    """
    Returns a list of supported date formats.

    Returns:
        formats: List of supported date formats
    """
    return mgp.Record(formats=list(DateFormatUtil.ISO_DATE_FORMATS.keys()))
