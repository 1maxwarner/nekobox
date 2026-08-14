#!/usr/bin/env python3
"""Build the compact Game Mod catalog and icon atlas from an ExitLag export."""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
import re
from pathlib import Path
from typing import Any

from PIL import Image, ImageDraw, ImageFont, ImageOps


ICON_SIZE = 48
ATLAS_COLUMNS = 28

PROFILE_ALIASES = {
    "counter-strike 2": {"cs2.exe", "csgos2.exe", "csgo.exe", "esportal-anti-cheat.exe"},
    "dead frontier 1": {"deadfrontier.exe"},
    "disney speedstorm": {"disney_speedstorm_x64_rtl.exe", "disneyspeedstorm.exe"},
    "overwatch 2": {"overwatch.exe"},
    "roblox studio": {"robloxstudiobeta.exe"},
    "starcraftii": {"starcraft.exe", "starcraft ii.exe"},
    "teamspeak3": {"ts3client_win32.exe", "ts3client_win64.exe"},
    "the division 2": {"thedivision2.exe"},
}

# Alternate clients that share the same routing endpoints as the primary
# application. They are added to the service rule as well as its search data.
SERVICE_PROCESS_ALIASES = {
    "telegram": {
        "telegram.exe": {"AyuGram.exe"},
    },
}

DISPLAY_NAMES = {
    "adobe": "Adobe",
    "chatgpt": "ChatGPT",
    "discord": "Discord",
    "facebook": "Facebook",
    "github": "GitHub",
    "google": "Google",
    "instagram": "Instagram",
    "linkedin": "LinkedIn",
    "microsoft": "Microsoft",
    "netflix": "Netflix",
    "openai": "OpenAI",
    "spotify": "Spotify",
    "steam": "Steam",
    "telegram": "Telegram",
    "tiktok": "TikTok",
    "twitch": "Twitch",
    "twitter": "X / Twitter",
    "whatsapp": "WhatsApp",
    "youtube": "YouTube",
}

GROUP_CATEGORIES = {
    "ai": "ai",
    "anime": "streaming",
    "art": "creative",
    "casino": "games",
    "discord": "messaging",
    "education": "education",
    "finance": "finance",
    "games": "games",
    "hosting": "tools",
    "jetbrains": "tools",
    "messengers": "messaging",
    "music": "streaming",
    "news": "news",
    "porn": "streaming",
    "shop": "shopping",
    "socials": "social",
    "tools": "tools",
    "torrent": "tools",
    "video": "streaming",
    "youtube": "streaming",
}

BRAND_CATEGORIES = {
    "adobe": "creative",
    "discord": "messaging",
    "facebook": "social",
    "google": "tools",
    "instagram": "social",
    "microsoft": "tools",
    "spotify": "streaming",
    "telegram": "messaging",
    "tiktok": "social",
    "twitch": "streaming",
    "twitter": "social",
    "whatsapp": "messaging",
    "youtube": "streaming",
}

PORTAL_BRAND_ALIASES = {
    "ea com": "ea",
    "steamcommunity": "steam",
    "steampowered": "steam",
    "steamstatic": "steam",
}

# These suffixes describe another build, region, or launcher for the same
# service. When the export contains more than one entry with the same base
# name, keep one card and combine all exact rules under it.
APPLICATION_VARIANT_SUFFIXES = {
    "beta",
    "br",
    "brazil",
    "china",
    "client",
    "cn",
    "eu",
    "europe",
    "experimental",
    "global",
    "international",
    "japan",
    "jp",
    "kr",
    "korea",
    "latam",
    "launcher",
    "mobile",
    "na",
    "pc",
    "ru",
    "russia",
    "russian",
    "sea",
    "taiwan",
    "test",
    "testing",
    "tw",
    "ua",
    "vn",
}


def unique(values: list[Any]) -> list[Any]:
    return list(dict.fromkeys(values))


def clean_process_name(value: str) -> str:
    return value.replace("\\ ", " ").strip()


def normalized(value: str) -> str:
    return " ".join(re.findall(r"[a-z0-9]+", value.casefold()))


def compact(value: str) -> str:
    return normalized(value).replace(" ", "")


def is_technical_application_name(value: str) -> bool:
    return re.fullmatch(r"[0-9a-f]{24,64}", value.strip(), re.IGNORECASE) is not None


def exact_application_display_name(
    application: dict[str, Any], profiles: dict[int, dict[str, Any]]
) -> str:
    original = application["application_name"].strip()
    if not is_technical_application_name(original):
        return original

    application_id = str(application["application_id"])
    candidates: list[str] = []
    for profile_index in application.get("profile_catalog_indexes", []):
        profile = profiles.get(profile_index, {})
        for metadata in profile.get("exact_memory_metadata", []):
            owner_ids = {
                str(metadata.get("application_id", "")),
                str(metadata.get("owner_id", "")),
            }
            service_name = str(metadata.get("service_name", "")).strip()
            if (
                application_id in owner_ids
                and service_name
                and not is_technical_application_name(service_name)
            ):
                candidates.append(service_name)

    unique_candidates = unique(candidates)
    if len({candidate.casefold() for candidate in unique_candidates}) == 1:
        return unique_candidates[0]
    return original


def service_aliases(name: str) -> list[str]:
    aliases = [normalized(name), compact(name)]
    if normalized(name) == "counter strike 2":
        aliases.extend(["cs2", "csgo", "counter strike", "counter-strike 2"])
    for application_name, executables in PROFILE_ALIASES.items():
        if normalized(name) == normalized(application_name):
            aliases.extend(
                re.sub(r"\.exe$", "", executable, flags=re.IGNORECASE)
                for executable in executables
            )
    return unique([value for value in aliases if value])


def application_family(name: str) -> str:
    tokens = normalized(name).split()
    while len(tokens) > 1 and tokens[-1] in APPLICATION_VARIANT_SUFFIXES:
        tokens.pop()
    return " ".join(tokens)


def coalesce_application_variants(
    applications: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    groups: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for application in applications:
        groups[application_family(application["name"])].append(application)

    result: list[dict[str, Any]] = []
    for family, variants in groups.items():
        # Prefer an unsuffixed entry so existing selections and its artwork
        # remain stable. Otherwise choose the shortest, then oldest numeric ID.
        primary = min(
            variants,
            key=lambda item: (
                normalized(item["name"]) != family,
                len(normalized(item["name"])),
                int(item["id"]) if str(item["id"]).isdigit() else math.inf,
            ),
        )
        combined = dict(primary)
        if normalized(primary["name"]) == family and family in DISPLAY_NAMES:
            combined["name"] = DISPLAY_NAMES.get(family, primary["name"])
        combined["profile_indexes"] = unique(
            [
                profile_index
                for variant in variants
                for profile_index in variant["profile_indexes"]
            ]
        )
        all_ids = unique(
            [
                str(application_id)
                for variant in variants
                for application_id in variant.get("application_ids", [variant["id"]])
            ]
        )
        combined["legacy_ids"] = [
            application_id
            for application_id in all_ids
            if application_id != str(combined["id"])
        ]
        combined["variant_names"] = unique(
            [
                variant_name
                for variant in variants
                for variant_name in variant.get("variant_names", [variant["name"]])
            ]
        )
        combined["variant_names"] = unique(
            combined["variant_names"] + [family, combined["name"]]
        )
        result.append(combined)
    return result


def category_for_service(name: str) -> str:
    value = normalized(name)
    if any(token in value for token in ("youtube", "twitch", "netflix", "spotify", "video", "music")):
        return "streaming"
    if any(token in value for token in ("discord", "telegram", "whatsapp", "messenger")):
        return "messaging"
    if any(token in value for token in ("facebook", "instagram", "twitter", "tiktok", "reddit")):
        return "social"
    if any(token in value for token in ("chatgpt", "claude", "deepseek", "gemini", "grok", "perplexity", "qwen")):
        return "ai"
    if any(token in value for token in (
        "brave", "chrome", "chromium", "edge", "firefox", "google",
        "github", "browser", "cloudflare", "librewolf", "opera", "vivaldi",
    )):
        return "tools"
    return "games"


def portal_brand(portal: str) -> str:
    value = portal.casefold()
    if "@" in value:
        prefix = normalized(value.split("@", 1)[0])
        if prefix:
            return PORTAL_BRAND_ALIASES.get(prefix, prefix)
    if "youtube" in value or "youtu.be" in value:
        return "youtube"
    if "twitch" in value:
        return "twitch"
    if "discord" in value:
        return "discord"
    if value in {"x.com", "twitter.com", "t.co"}:
        return "twitter"
    if value.endswith(".google") or "google.com" in value or "withgoogle.com" in value:
        return "google"
    stem = value.split(".", 1)[0]
    brand = normalized(stem) or normalized(value)
    return PORTAL_BRAND_ALIASES.get(brand, brand)


def portal_display_name(portal: str, brand: str) -> str:
    if brand in DISPLAY_NAMES:
        return DISPLAY_NAMES[brand]
    return portal


def collapse_domains(values: list[str]) -> list[str]:
    cleaned = {
        value.casefold().strip().lstrip("*.").rstrip(".")
        for value in values
        if isinstance(value, str) and "." in value
    }
    result: list[str] = []
    for domain in sorted(cleaned, key=lambda value: (value.count("."), len(value), value)):
        if any(domain == parent or domain.endswith("." + parent) for parent in result):
            continue
        result.append(domain)
    return result


def load_portal_categories(config_root: Path | None) -> dict[str, str]:
    if config_root is None or not config_root.exists():
        return {}
    result: dict[str, str] = {}
    for path in config_root.glob("*/*.json"):
        result[path.stem.casefold()] = GROUP_CATEGORIES.get(path.parent.name.casefold(), "other")
    return result


def generic_icon(name: str) -> Image.Image:
    digest = hashlib.sha256(name.encode("utf-8")).digest()
    background = (64 + digest[0] // 2, 64 + digest[1] // 2, 64 + digest[2] // 2, 255)
    tile = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
    badge = Image.new("RGBA", (ICON_SIZE - 8, ICON_SIZE - 8), (0, 0, 0, 0))
    ImageDraw.Draw(badge).rounded_rectangle(
        (0, 0, badge.width - 1, badge.height - 1), radius=10, fill=background
    )
    tile.alpha_composite(badge, (4, 4))
    draw = ImageDraw.Draw(tile)
    letter = next((character.upper() for character in name if character.isalnum()), "?")
    font = ImageFont.load_default(size=20)
    box = draw.textbbox((0, 0), letter, font=font)
    draw.text(
        ((ICON_SIZE - (box[2] - box[0])) / 2, (ICON_SIZE - (box[3] - box[1])) / 2 - 1),
        letter,
        fill=(255, 255, 255, 245),
        font=font,
    )
    return tile


def normalized_icon(original: Image.Image) -> Image.Image:
    """Trim transparent padding and fit every logo into the same 40px box."""
    icon = original.convert("RGBA")
    bounds = icon.getchannel("A").getbbox()
    if bounds is not None:
        icon = icon.crop(bounds)
    return ImageOps.contain(
        icon,
        (ICON_SIZE - 8, ICON_SIZE - 8),
        Image.Resampling.LANCZOS,
    )


def opencck_icon(
    icon_root: Path | None, portal_names: list[str], display_name: str
) -> Image.Image:
    if icon_root is not None and icon_root.exists():
        candidates = unique(
            [*portal_names, display_name.casefold(), normalized(display_name), compact(display_name)]
        )
        supported = {".png", ".ico", ".jpg", ".jpeg", ".webp"}
        paths = [path for path in icon_root.iterdir() if path.suffix.casefold() in supported]
        by_stem = {path.stem.casefold(): path for path in paths}
        for candidate in candidates:
            path = by_stem.get(candidate.casefold())
            if path is None:
                continue
            try:
                with Image.open(path) as original:
                    icon = normalized_icon(original)
                tile = Image.new("RGBA", (ICON_SIZE, ICON_SIZE), (0, 0, 0, 0))
                tile.alpha_composite(
                    icon, ((ICON_SIZE - icon.width) // 2, (ICON_SIZE - icon.height) // 2)
                )
                return tile
            except (OSError, ValueError):
                continue
    return generic_icon(display_name)


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
            # sing-box uses a colon for inclusive port ranges.
            port_ranges.append(f"{start}:{end}")

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


def add_service_process_aliases(
    service_name: str, rules: list[dict[str, Any]], keywords: list[str]
) -> None:
    aliases = SERVICE_PROCESS_ALIASES.get(normalized(service_name), {})
    if not aliases:
        return

    keywords.extend(alias for values in aliases.values() for alias in values)
    for rule in rules:
        processes = rule.get("process_name")
        if not isinstance(processes, list):
            continue
        additions = [
            alias
            for original, values in aliases.items()
            if any(value.casefold() == original for value in processes)
            for alias in values
        ]
        if additions:
            rule["process_name"] = unique(processes + additions)


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def build(
    source: Path,
    output: Path,
    opencck_files: list[Path] | None = None,
    opencck_config_root: Path | None = None,
    opencck_icons_root: Path | None = None,
) -> None:
    applications = load_json(source / "applications.json")
    catalog = load_json(source / "catalog.json")
    profiles = {profile["catalog_index"]: profile for profile in catalog["profiles"]}

    merged: dict[str, dict[str, Any]] = {}
    for application in applications:
        profile_indexes = application.get("profile_catalog_indexes", [])
        if not application.get("icon_copied"):
            continue
        original_name = application["application_name"].strip()
        name = exact_application_display_name(application, profiles)
        key = name.casefold()
        entry = merged.setdefault(
            key,
            {
                "id": str(application["application_id"]),
                "name": name,
                "icon_file": application["icon_file"],
                "profile_indexes": [],
                "application_ids": [],
                "variant_names": [],
                "technical_source": is_technical_application_name(original_name),
            },
        )
        if entry["technical_source"] and not is_technical_application_name(
            original_name
        ):
            entry.update(
                {
                    "id": str(application["application_id"]),
                    "name": name,
                    "icon_file": application["icon_file"],
                    "technical_source": False,
                }
            )
        entry["profile_indexes"] = unique(entry["profile_indexes"] + profile_indexes)
        entry["application_ids"] = unique(
            entry["application_ids"] + [str(application["application_id"])]
        )
        entry["variant_names"] = unique(entry["variant_names"] + [name])

    # Some ExitLag application records have icons and names but lost the
    # application-id link to their executable profile. Restore exact stem
    # matches and a small set of well-known aliases before dropping them.
    application_keys: dict[str, str] = {}
    for key, application in merged.items():
        for candidate in (normalized(application["name"]), compact(application["name"])):
            application_keys[candidate] = key
    for profile in catalog["profiles"]:
        if profile.get("application_id") is not None:
            continue
        processes = {
            clean_process_name(value).casefold()
            for values in profile.get("definitions", {}).values()
            for value in values
        }
        profile_name = re.sub(
            r"\.(?:exe|bin|atm|dat|xem|sw|x32|x64)$",
            "",
            profile.get("application_name", ""),
            flags=re.IGNORECASE,
        )
        target_key = application_keys.get(normalized(profile_name)) or application_keys.get(compact(profile_name))
        if target_key is None:
            process_stems = {
                re.sub(
                    r"\.(?:exe|bin|atm|dat|xem|sw|x32|x64)$",
                    "",
                    process,
                    flags=re.IGNORECASE,
                )
                for process in processes
                if not any(character in process for character in "*?")
            }
            exact_targets = {
                application_keys[candidate]
                for process in process_stems
                for candidate in (normalized(process), compact(process))
                if candidate in application_keys
            }
            if len(exact_targets) == 1:
                target_key = exact_targets.pop()
        if target_key is None:
            for application_name, aliases in PROFILE_ALIASES.items():
                if processes.intersection(aliases):
                    target_key = application_keys.get(normalized(application_name))
                    break
        if target_key in merged:
            merged[target_key]["profile_indexes"] = unique(
                merged[target_key]["profile_indexes"] + [profile["catalog_index"]]
            )

    source_entries = sorted(
        coalesce_application_variants(list(merged.values())),
        key=lambda item: item["name"].casefold(),
    )
    unresolved_names = [
        entry["name"]
        for entry in source_entries
        if is_technical_application_name(entry["name"])
        and entry["profile_indexes"]
    ]
    if unresolved_names:
        raise ValueError(
            "Exact service names are missing for technical application records: "
            + ", ".join(unresolved_names[:20])
        )
    services: list[dict[str, Any]] = []
    opencck_files = opencck_files or []
    portal_count = sum(len(load_json(path)) for path in opencck_files if path.exists())
    atlas_rows = math.ceil((len(source_entries) + portal_count) / ATLAS_COLUMNS)
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

        add_service_process_aliases(application["name"], rules, keywords)

        if not rules:
            continue

        atlas_index = len(services)
        icon_path = source / application["icon_file"]
        with Image.open(icon_path) as original:
            icon = normalized_icon(original)
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
                "aliases": unique(
                    service_aliases(application["name"])
                    + [
                        alias
                        for variant_name in application.get("variant_names", [])
                        for alias in [
                            variant_name,
                            normalized(variant_name),
                            compact(variant_name),
                        ]
                        if alias
                    ]
                ),
                "legacy_ids": application.get("legacy_ids", []),
                "category": category_for_service(application["name"]),
                "source": "exitlag",
                "direct_rule_count": direct_count,
                "proxy_rule_count": proxy_count,
                "rules": rules,
            }
        )

    # A compact launcher-only route requested for League of Legends. Keep the
    # executable names as search metadata; the actual match intentionally uses
    # only the supplied network so launcher updates are covered as well.
    services.append(
        {
            "id": "manual:league-launcher",
            "name": "League Launcher",
            "icon": [],
            "keywords": [
                "LeagueClient.exe",
                "LeagueClientUx.exe",
                "RiotClientServices.exe",
                "RiotClientUx.exe",
            ],
            "aliases": [
                "league launcher",
                "league of legends launcher",
                "lol launcher",
                "riot launcher",
            ],
            "legacy_ids": [],
            "category": "games",
            "source": "manual",
            "direct_rule_count": 0,
            "proxy_rule_count": 1,
            "rules": [
                {
                    "action": "route",
                    "outbound": "proxy",
                    "ip_cidr": ["3.64.0.0/12"],
                }
            ],
        }
    )

    portal_categories = load_portal_categories(opencck_config_root)
    service_match: dict[str, dict[str, Any]] = {}
    domain_owner: dict[str, dict[str, Any]] = {}
    for service in services:
        for candidate in [service["name"], *service.get("aliases", [])]:
            service_match[normalized(candidate)] = service
            service_match[compact(candidate)] = service
        for rule in service["rules"]:
            for key in ("domain", "domain_suffix"):
                for domain in rule.get(key, []):
                    domain_owner.setdefault(domain.casefold(), service)

    opencck_services: dict[str, dict[str, Any]] = {}
    for opencck_path in opencck_files:
        if not opencck_path.exists():
            continue
        for portal, raw_domains in load_json(opencck_path).items():
            if not raw_domains:
                continue
            brand = portal_brand(portal)
            match_candidates = [normalized(portal), compact(portal), normalized(brand), compact(brand)]
            target = next((service_match.get(candidate) for candidate in match_candidates if candidate in service_match), None)
            if target is None:
                target = opencck_services.get(brand)
            if target is None:
                target = {
                    "id": f"opencck:{brand}",
                    "name": portal_display_name(portal, brand),
                    "icon": [],
                    "keywords": [],
                    "aliases": unique([portal, brand, normalized(portal), compact(portal)]),
                    "legacy_ids": [],
                    "category": BRAND_CATEGORIES.get(
                        brand, portal_categories.get(portal.casefold(), "other")
                    ),
                    "source": "opencck",
                    "direct_rule_count": 0,
                    "proxy_rule_count": 1,
                    "rules": [],
                    "_domains": [],
                    "_portals": [],
                }
                opencck_services[brand] = target
                services.append(target)
                for candidate in match_candidates:
                    service_match[candidate] = target

            owned_domains: list[str] = target.setdefault("_domains", [])
            for domain in collapse_domains(raw_domains):
                owner = domain_owner.get(domain)
                if owner is not None and owner is not target:
                    continue
                if domain not in owned_domains:
                    owned_domains.append(domain)
                    domain_owner[domain] = target
            target["aliases"] = unique(target.get("aliases", []) + [portal, brand])
            target.setdefault("_portals", []).append(portal)
            if target.get("category") in {None, "other"}:
                target["category"] = BRAND_CATEGORIES.get(
                    brand, portal_categories.get(portal.casefold(), "other")
                )

    for service in services:
        domains = collapse_domains(service.pop("_domains", []))
        if domains:
            # OpenCCK rules are ordinary service rules. Stable and beta inputs
            # are merged into the same domain set with no separate priority.
            service["rules"].append(
                {
                    "action": "route",
                    "outbound": "proxy",
                    "domain_suffix": domains,
                }
            )
            service["proxy_rule_count"] = sum(
                rule.get("outbound") == "proxy" for rule in service["rules"]
            )

    # Allocate generic tiles for services introduced by OpenCCK. The original
    # ExitLag icons keep their existing atlas positions.
    for service in services:
        if service["icon"]:
            service.pop("_portals", None)
            continue
        atlas_index = max(
            [rect[1] // ICON_SIZE * ATLAS_COLUMNS + rect[0] // ICON_SIZE for rect in (item["icon"] for item in services) if rect]
            + [-1]
        ) + 1
        column = atlas_index % ATLAS_COLUMNS
        row = atlas_index // ATLAS_COLUMNS
        atlas.alpha_composite(
            opencck_icon(
                opencck_icons_root,
                unique(service.pop("_portals", [])),
                service["name"],
            ),
            (column * ICON_SIZE, row * ICON_SIZE),
        )
        service["icon"] = [column * ICON_SIZE, row * ICON_SIZE, ICON_SIZE, ICON_SIZE]

    services.sort(key=lambda item: (item.get("category", "other"), item["name"].casefold()))

    output.mkdir(parents=True, exist_ok=True)
    atlas.crop((0, 0, atlas.width, math.ceil(len(services) / ATLAS_COLUMNS) * ICON_SIZE)).save(
        output / "icons.png", optimize=True
    )
    payload = {"version": 2, "service_count": len(services), "services": services}
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
    parser.add_argument(
        "--opencck",
        action="append",
        default=[],
        type=Path,
        help="OpenCCK JSON domain snapshot (may be repeated; earlier files win)",
    )
    parser.add_argument(
        "--opencck-config-root",
        type=Path,
        help="Optional OpenCCK config directory used to derive categories",
    )
    parser.add_argument(
        "--opencck-icons-root",
        type=Path,
        help="Optional OpenCCK icon directory used by website-only services",
    )
    args = parser.parse_args()
    build(
        args.source,
        args.output,
        args.opencck,
        args.opencck_config_root,
        args.opencck_icons_root,
    )


if __name__ == "__main__":
    main()
