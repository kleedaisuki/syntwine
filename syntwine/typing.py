"""Value and callback types for the native Syntwine API."""

from collections.abc import Callable, Mapping
from datetime import date, datetime, time
from os import PathLike
from typing import TypedDict

type PathArgument = str | bytes | PathLike[str] | PathLike[bytes]
type JSONValue = (
    None | bool | int | float | str | list[JSONValue] | dict[str, JSONValue]
)
type TOMLValue = (
    bool
    | int
    | float
    | str
    | datetime
    | date
    | time
    | list[TOMLValue]
    | dict[str, TOMLValue]
)
type TOMLTable = dict[str, TOMLValue]
type BuildContext = Mapping[str, object]
type Serializer[T] = Callable[[T], bytes]
type Parser[T] = Callable[[bytes], T]
type Validator[T] = Callable[[T], bool | None]


class MarkdownHeading(TypedDict):
    title: str
    level: int
    line: int


class MarkdownSection(MarkdownHeading):
    source: str


class MarkdownLink(TypedDict):
    label: str
    url: str
    title: str
    image: bool
