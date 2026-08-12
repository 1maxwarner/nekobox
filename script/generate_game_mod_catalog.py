#!/usr/bin/env python3
"""Build the compact Game Mod catalog and icon atlas from an ExitLag export."""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
from typing import Any

from PIL import Image, ImageOps


ICON_SIZE = 48
ATLAS_COLUMNS = 28


def unique(values: list[Any]) -> list[Any]:
    return list(dict.fromkeys(values))


def clean_process_name(value: str) -> str:
    return value.replace("\\ ", " ").strip()


def expand_processes(route: dict[str, Any], definitions: dict[str, list[str]]) -> list[str]:
    result: list[str] = []
    for value in route.get("files", []):
        if value.startswith("$"):
            result.extend(definitions.get(value[1:], []))
        else:
            result.append(value)
    return unique([clean_process_name(value) for value in result if value.strip()])


def convert_rule(route: dict[str, Any], definitions: dict[str, list[str]]) -> dict[str, Any] | None:
    action = route.get("action", "").upper()
    if action not in {"PASS", "PROXIFY"}:
        return None

    processes = expand_processes(route, definitions)
    # Process scoping is the safety boundary: never turn a malformed catalog
    # entry into a rule that captures unrelated system traffic.
    if not processes:
        return None

    result: dict[str, Any] = {
        "action": "route",
        "outbound": "direct" if action == "PASS" else "proxy",
        "process_name": processes,
    }

    exact_domains: list[str] = []
    suffix_domains: list[str] = []
    for domain in route.get("domains", []):
        domain = domain.strip()
        if domain.startswith("*."):
            suffix_domains.append(domain[2:])
        elif domain:
            exact_domains.append(domain)
    if exact_domains:
        result["domain"] = unique(exact_domains)
    if suffix_domains:
        result["domain_suffix"] = unique(suffix_domains)

    networks = unique([value.strip() for value in route.get("remote_networks", []) if value.strip()])
    if networks:
        result["ip_cidr"] = networks

    ports: list[int] = []
    port_ranges: list[str] = []

    def append_port_span(start: int, end: int) -> None:
        start = max(start, 1)
        end = min(end, 65535)
        if start > end:
            return
        if start == end:
            ports.append(start)
        else:
            port_ranges.append(f"{start}-{end}")

    for value in route.get("remote_ports", []):
        value = str(value).strip()
        if not value:
            continue
        if "-" in value:
            span = re.match(r"^(\d+)\s*-\s*(\d+)", value)
            if span is None:
                continue
            start, end = map(int, span.groups())
            excluded = re.search(r"\bexcept\D*(\d+)", value, re.IGNORECASE)
            if excluded is None or not start <= int(excluded.group(1)) <= end:
                append_port_span(start, end)
            else:
                excluded_port = int(excluded.group(1))
                append_port_span(start, excluded_port - 1)
                append_port_span(excluded_port + 1, end)
        elif value.isdigit():
            port = int(value)
            if 1 <= port <= 65535:
                ports.append(port)
    if ports:
        result["port"] = unique(ports)
    if port_ranges:
        result["port_range"] = unique(port_ranges)

    # Text after VIA describes the catalog's tunnel transport, not the
    # protocol to match. Only inspect the rule portion before VIA.
    header = route.get("header", "").upper().split(" VIA ", 1)[0]
    networks_used = [name.lower() for name in ("TCP", "UDP") if name in header]
    if len(networks_used) == 1:
        result["network"] = networks_used[0]
    elif len(networks_used) == 2:
        result["network"] = networks_used

    return result


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def build(source: Path, output: Path) -> None:
    applications = load_json(source / "applications.json")
    catalog = load_json(source / "catalog.json")
    profiles = {profile["catalog_index"]: profile for profile in catalog["profiles"]}

    merged: dict[str, dict[str, Any]] = {}
    for application in applications:
        profile_indexes = application.get("profile_catalog_indexes", [])
        if not profile_indexes or not application.get("icon_copied"):
            continue
        name = application["application_name"].strip()
        key = name.casefold()
        entry = merged.setdefault(
            key,
            {
                "id": str(application["application_id"]),
                "name": name,
                "icon_file": application["icon_file"],
                "profile_indexes": [],
            },
        )
        entry["profile_indexes"] = unique(entry["profile_indexes"] + profile_indexes)

    source_entries = sorted(merged.values(), key=lambda item: item["name"].casefold())
    services: list[dict[str, Any]] = []
    atlas_rows = math.ceil(len(source_entries) / ATLAS_COLUMNS)
    atlas = Image.new("RGBA", (ATLAS_COLUMNS * ICON_SIZE, atlas_rows * ICON_SIZE), (0, 0, 0, 0))

    for application in source_entries:
        rules: list[dict[str, Any]] = []
        keywords: list[str] = []
        for profile_index in application["profile_indexes"]:
            profile = profiles.get(profile_index)
            if profile is None:
                continue
            definitions = profile.get("definitions", {})
            for values in definitions.values():
                keywords.extend(clean_process_name(value) for value in values)
            for route in profile.get("routes", []):
                converted = convert_rule(route, definitions)
                if converted is not None and converted not in rules:
                    rules.append(converted)

        if not rules:
            continue

        atlas_index = len(services)
        icon_path = source / application["icon_file"]
        with Image.open(icon_path) as original:
            icon = ImageOps.contain(original.convert("RGBA"), (ICON_SIZE - 8, ICON_SIZE - 8), Image.Resampling.LANCZOS)
        tile = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
        tile.alpha_composite(icon, ((ICON_SIZE - icon.width) // 2, (ICON_SIZE - icon.height) // 2))
        column = atlas_index % ATLAS_COLUMNS
        row = atlas_index // ATLAS_COLUMNS
        atlas.alpha_composite(tile, (column * ICON_SIZE, row * ICON_SIZE))

        direct_count = sum(rule["outbound"] == "direct" for rule in rules)
        proxy_count = sum(rule["outbound"] == "proxy" for rule in rules)
        services.append(
            {
                "id": application["id"],
                "name": application["name"],
                "icon": [column * ICON_SIZE, row * ICON_SIZE, ICON_SIZE, ICON_SIZE],
                "keywords": unique([value for value in keywords if value]),
                "direct_rule_count": direct_count,
                "proxy_rule_count": proxy_count,
                "rules": rules,
            }
        )

    output.mkdir(parents=True, exist_ok=True)
    atlas.crop((0, 0, atlas.width, math.ceil(len(services) / ATLAS_COLUMNS) * ICON_SIZE)).save(
        output / "icons.png", optimize=True
    )
    payload = {"version": 1, "service_count": len(services), "services": services}
    (output / "catalog.json").write_text(
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")), encoding="utf-8"
    )
    print(f"Generated {len(services)} services in {output}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="ExitLag catalog export directory")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("res/public/game_mod"),
        help="Output directory (default: res/public/game_mod)",
    )
    args = parser.parse_args()
    build(args.source, args.output)


if __name__ == "__main__":
    main()
