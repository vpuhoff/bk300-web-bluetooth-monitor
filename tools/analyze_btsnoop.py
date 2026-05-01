#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Разбор Android HCI snoop (btsnoop_hci.log) с акцентом на BLE ATT (GATT).

Требования:
  - Установлен Wireshark / tshark в PATH (Linux: apt install tshark).
  - pip install -r tools/requirements-btsnoop.txt

Примеры:
  python3 tools/analyze_btsnoop.py btsnoop_hci.log
  python3 tools/analyze_btsnoop.py btsnoop_hci.log --summary
  python3 tools/analyze_btsnoop.py btsnoop_hci.log --l2cap --max 50
  python3 tools/analyze_btsnoop.py btsnoop_hci.log --handles 0x25 0x26 0x28
  python3 tools/analyze_btsnoop.py btsnoop_hci.log --raw-fields
  python3 tools/analyze_btsnoop.py btsnoop_hci.log.last --detect-handles --emit-cpp

Если ATT-пусто: линк часто зашифрован — Wireshark не строит btatt; смотрите --l2cap и ключи в Wireshark.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter

def _pkt_no(pkt) -> int:
    """Pyshark иногда отдаёт number как нестандартный тип."""
    n = getattr(pkt, "number", 0)
    try:
        return int(n)
    except (TypeError, ValueError):
        return int(str(n))


def _pkt_ts_iso(pkt) -> str:
    ts = getattr(pkt, "sniff_time", None)
    if ts is not None:
        try:
            return ts.isoformat()
        except Exception:
            pass
    st = getattr(pkt, "sniff_timestamp", None)
    return str(st) if st is not None else ""


ATT_OPCODE_NAMES = {
    0x01: "Error Response",
    0x02: "Exchange MTU Request",
    0x03: "Exchange MTU Response",
    0x04: "Find Information Request",
    0x05: "Find Information Response",
    0x06: "Find By Type Value Request",
    0x07: "Find By Type Value Response",
    0x08: "Read By Type Request",
    0x09: "Read By Type Response",
    0x0A: "Read Request",
    0x0B: "Read Response",
    0x0C: "Read Blob Request",
    0x0D: "Read Blob Response",
    0x0E: "Read Multiple Request",
    0x0F: "Read Multiple Response",
    0x10: "Read By Group Type Request",
    0x11: "Read By Group Type Response",
    0x12: "Write Request",
    0x13: "Write Response",
    0x16: "Prepare Write Request",
    0x17: "Prepare Write Response",
    0x18: "Execute Write Request",
    0x19: "Execute Write Response",
    0x1B: "Handle Value Notification",
    0x1D: "Handle Value Indication",
    0x1E: "Handle Value Confirmation",
    0x52: "Write Command",
}


def _hex_parse(s: str | None) -> int | None:
    if s is None:
        return None
    s = str(s).strip()
    if not s:
        return None
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16)
    # Wireshark иногда даёт "41:42:43" или чистые hex без префикса
    if ":" in s:
        return None
    try:
        return int(s, 16)
    except ValueError:
        try:
            return int(s, 10)
        except ValueError:
            return None


def _normalize_handle(val: str | None) -> int | None:
    if val is None:
        return None
    v = _hex_parse(val)
    if v is not None:
        return v
    # иногда "handle (0x0028)"
    m = re.search(r"0x([0-9a-fA-F]+)", str(val))
    if m:
        return int(m.group(1), 16)
    return None


def _layer_fields(layer) -> dict[str, str]:
    """Pyshark хранит поля в _all_fields (dict) или через field_names."""
    out: dict[str, str] = {}
    raw = getattr(layer, "_all_fields", None)
    if isinstance(raw, dict):
        for k, v in raw.items():
            out[str(k)] = str(v) if v is not None else ""
        return out
    try:
        for name in layer.field_names:
            try:
                out[name] = layer.get_field(name)
            except Exception:
                out[name] = "?"
    except Exception:
        pass
    return out


def _opcode_from_text(raw: str) -> int | None:
    """Иногда Wireshark пишет 'Write Command (0x52)' без отдельного hex поля."""
    if not raw:
        return None
    m = re.search(r"\(0x([0-9a-fA-F]{1,2})\)", raw, re.I)
    if m:
        return int(m.group(1), 16)
    return _hex_parse(raw.split(",")[0].strip())


def _pick_opcode(fields: dict[str, str]) -> tuple[int | None, str]:
    """Wireshark версии по-разному называют поля."""
    for key in (
        "btatt.opcode",
        "btatt.opcode_tree",
        "bluetooth.att.opcode",
    ):
        if key in fields:
            raw = fields[key]
            n = _opcode_from_text(raw) or _hex_parse(raw.split()[0] if raw else None)
            if n is not None:
                return n, raw
    # иногда opcode только в составном дереве — перебор всех ключей
    for k, v in fields.items():
        if "opcode" in k.lower() and "cmd" not in k.lower():
            n = _opcode_from_text(str(v)) or _hex_parse(str(v).split()[0])
            if n is not None:
                return n, v
    return None, ""


def _pick_handle(fields: dict[str, str]) -> int | None:
    for key in (
        "btatt.handle",
        "btatt.handle_tree",
        "btatt.handle_uuid16",
        "bluetooth.att.handle",
    ):
        if key in fields:
            h = _normalize_handle(fields[key])
            if h is not None:
                return h
    for k, v in fields.items():
        if k.endswith(".handle") or k == "btatt.handle":
            h = _normalize_handle(v)
            if h is not None:
                return h
    return None


def _pick_value_hex(fields: dict[str, str]) -> str:
    for key in (
        "btatt.value",
        "btatt.characteristic_value",
        "bluetooth.att.value",
    ):
        if key in fields and fields[key]:
            return fields[key].replace(":", "").lower()
    # любое поле со словом value / characteristic_value
    for k, v in sorted(fields.items()):
        lk = k.lower()
        if ("value" in lk or "characteristic" in lk) and v and len(str(v)) > 2:
            return str(v).replace(":", "").lower()
    # Wireshark иногда кладёт только «aa:bb:cc» в произвольное btatt.* без "value"
    for k, v in sorted(fields.items()):
        if not str(k).startswith("btatt.") or not v:
            continue
        compact = _norm_hex_sub(str(v)).lower()
        if len(compact) >= 4 and len(compact) % 2 == 0:
            if all(c in "0123456789abcdef" for c in compact):
                return compact
    return ""


def _norm_hex_sub(s: str) -> str:
    return re.sub(r"[^0-9a-fA-F]", "", s)


def _counter_best(cnt: Counter) -> tuple[int | None, int]:
    if not cnt:
        return None, 0
    h, c = cnt.most_common(1)[0]
    return h, c


def run_detect_handles(
    capture_path: str,
    pyshark_mod,
    max_packets: int,
) -> dict[str, int | Counter]:
    """
    Выводит статистику по ATT для подстановки handle на ESP (или сверки с discovery).

    Эвристики:
      - Notify value handle — ATT Handle Value Notification (0x1B).
      - Write handle — Write Command (0x52); надёжнее с payload, начинающимся с 4040 (кадр BK300).
      - CCCD — Write Request (0x12) со значением 0100/0200; иначе handle notify+1, если на него были WR.
    """
    notify_h: Counter = Counter()
    write52_h: Counter = Counter()
    write52_bk300_h: Counter = Counter()
    cccd_wr_h: Counter = Counter()
    write12_h: Counter = Counter()

    cap = pyshark_mod.FileCapture(capture_path, display_filter="btatt", keep_packets=False)
    scanned = 0
    try:
        for pkt in cap:
            scanned += 1
            if max_packets and scanned > max_packets:
                break
            if not hasattr(pkt, "btatt"):
                continue
            fields = _layer_fields(pkt.btatt)
            opcode_n, _ = _pick_opcode(fields)
            handle = _pick_handle(fields)
            value_hex = _norm_hex_sub(_pick_value_hex(fields)).lower()

            if opcode_n == 0x1B and handle is not None:
                notify_h[handle] += 1
            if opcode_n == 0x52 and handle is not None:
                write52_h[handle] += 1
                if value_hex.startswith("4040"):
                    write52_bk300_h[handle] += 1
            if opcode_n == 0x12 and handle is not None:
                write12_h[handle] += 1
                if value_hex:
                    # Client Characteristic Configuration: notify 0x0001, indicate 0x0002 (LE в кадре)
                    if value_hex.startswith("0100") or value_hex.startswith("0200"):
                        cccd_wr_h[handle] += 1
    finally:
        cap.close()

    return {
        "scanned": scanned,
        "notify": notify_h,
        "write_cmd": write52_h,
        "write_cmd_bk300": write52_bk300_h,
        "cccd_write_req": cccd_wr_h,
        "write_req_any": write12_h,
    }


def print_detect_handles_report(res: dict[str, int | Counter], emit_cpp: bool) -> None:
    notify_h = res["notify"]
    write52_h = res["write_cmd"]
    bk300_h = res["write_cmd_bk300"]
    cccd_h = res["cccd_write_req"]
    wr_any = res["write_req_any"]

    nh, nc = _counter_best(notify_h)
    wh, wc = _counter_best(bk300_h)
    if wh is None:
        wh, wc = _counter_best(write52_h)
    ch, cc = _counter_best(cccd_h)
    cccd_note = ""
    if ch is None and nh is not None:
        cand = nh + 1
        if wr_any.get(cand, 0) > 0:
            ch, cc = cand, wr_any[cand]
            cccd_note = " (эвристика: notify_handle+1 и есть Write Request — типичный CCCD)"

    print("=== detect-handles (эвристики по ATT) ===\n")
    print(f"Просмотрено ATT-пакетов: {res['scanned']}\n")

    def _show_counter(title: str, cnt: Counter, limit: int = 5) -> None:
        print(f"{title}")
        if not cnt:
            print("  (нет)")
            return
        for h, c in cnt.most_common(limit):
            print(f"  0x{h:04x}  ×{c}")
        print()

    _show_counter("Handle Value Notification (0x1B) — значение характеристики notify:", notify_h)
    _show_counter("Write Command (0x52), payload 4040… — запись BK300:", bk300_h)
    _show_counter("Write Command (0x52) — все:", write52_h)
    _show_counter("Write Request (0x12), val 0100/0200 — вероятный CCCD:", cccd_h)
    _show_counter("Write Request (0x12) — все handle:", wr_any)

    print("Итог (наиболее частые):")
    print(f"  NOTIFY value handle:     {('0x%04x' % nh) if nh is not None else '(не найден)'}  (×{nc})")
    print(f"  WRITE handle (BK300):    {('0x%04x' % wh) if wh is not None else '(не найден)'}  (×{wc})")
    print(
        f"  CCCD handle (подписка): {('0x%04x' % ch) if ch is not None else '(не найден — ищите Write Req к дескриптору)'}  (×{cc})"
        + cccd_note
    )
    print()
    print(
        "Подсказка: на ESP32 удобнее искать характеристики по UUID FFF1/FFF2; "
        "эти числа — для сверки с логом discovery и отладки.",
    )

    if emit_cpp:
        print("\n--- фрагмент для копирования (только отладка; UUID надёжнее) ---\n")
        if nh is not None:
            print(f"#define BK300_ATT_HANDLE_NOTIFY_VALUE  0x{nh:04x}u  // Handle Value Notification")
        if wh is not None:
            print(f"#define BK300_ATT_HANDLE_WRITE         0x{wh:04x}u  // Write Command, кадры 4040…")
        if ch is not None:
            print(
                f"#define BK300_ATT_HANDLE_CCCD          0x{ch:04x}u  // Subscribe (descriptor)"
            )


def run_summary(capture_path: str, pyshark_mod, max_packets: int) -> dict[str, int]:
    stats = {"packets": 0, "btatt": 0, "btl2cap": 0, "bthci_acl": 0}
    cap = pyshark_mod.FileCapture(capture_path, keep_packets=False)
    try:
        for pkt in cap:
            stats["packets"] += 1
            if max_packets and stats["packets"] > max_packets:
                break
            if hasattr(pkt, "btatt"):
                stats["btatt"] += 1
            if hasattr(pkt, "btl2cap"):
                stats["btl2cap"] += 1
            if hasattr(pkt, "bthci_acl"):
                stats["bthci_acl"] += 1
    finally:
        cap.close()
    return stats


def run_l2cap_dump(
    capture_path: str,
    pyshark_mod,
    *,
    max_packets: int,
    grep_hex: str | None,
) -> tuple[int, int]:
    """Дамп поля btl2cap (cid, payload). Возвращает (просмотрено пакетов, строк напечатано)."""
    needle = _norm_hex_sub(grep_hex).lower() if grep_hex else None
    cap = pyshark_mod.FileCapture(capture_path, keep_packets=False)
    scanned = 0
    rows = 0
    try:
        for pkt in cap:
            scanned += 1
            if max_packets and scanned > max_packets:
                break
            if not hasattr(pkt, "btl2cap"):
                continue
            lay = pkt.btl2cap
            fields = _layer_fields(lay)
            cid = fields.get("btl2cap.cid", "?")
            payload = fields.get("btl2cap.payload", "")
            compact = _norm_hex_sub(payload).lower()
            if needle and needle not in compact:
                continue
            ts = getattr(pkt, "sniff_time", None)
            ts_s = ts.isoformat() if ts is not None else ""
            chandle = ""
            if hasattr(pkt, "bthci_acl"):
                acl_f = _layer_fields(pkt.bthci_acl)
                chandle = acl_f.get("bthci_acl.chandle", "")
            line = (
                f"{_pkt_no(pkt):6d}  {ts_s:26s}  chandle={chandle:8s}  cid={cid:10s}  "
                f"pay_len={len(compact)//2}  {compact[:100]}"
            )
            print(line)
            if len(compact) > 100:
                print(f"{'':8}{compact[100:200]}")
            rows += 1
    finally:
        cap.close()
    return scanned, rows


def main() -> int:
    ap = argparse.ArgumentParser(description="ATT-трафик из btsnoop (pyshark/tshark).")
    ap.add_argument("capture", help="Путь к btsnoop_hci.log")
    ap.add_argument(
        "--handles",
        nargs="*",
        help="Фильтр по handle (например 0x25 37 0x28); пусто — все ATT",
    )
    ap.add_argument("--max", type=int, default=0, help="Максимум пакетов (0 = без лимита)")
    ap.add_argument(
        "--display-filter",
        default="btatt",
        help="Фильтр Wireshark (по умолчанию только слой ATT)",
    )
    ap.add_argument(
        "--no-filter",
        action="store_true",
        help="Читать весь capture без display_filter (медленнее)",
    )
    ap.add_argument(
        "--raw-fields",
        action="store_true",
        help="Печатать все поля btatt для отладки имён полей",
    )
    ap.add_argument(
        "--summary",
        action="store_true",
        help="Только счётчики: пакеты, наличие btatt / btl2cap / ACL",
    )
    ap.add_argument(
        "--l2cap",
        action="store_true",
        help="Дамп L2CAP payload (когда btatt отсутствует — типично для шифрованного BLE)",
    )
    ap.add_argument(
        "--grep-hex",
        metavar="HEX",
        help="С --l2cap: оставить кадры, где payload содержит подстроку hex (без пробелов)",
    )
    ap.add_argument(
        "--detect-handles",
        action="store_true",
        help="Угадать NOTIFY / WRITE / CCCD handles из ATT (нужен декодированный btatt)",
    )
    ap.add_argument(
        "--emit-cpp",
        action="store_true",
        help="С --detect-handles: напечатать #define для отладки",
    )
    args = ap.parse_args()

    if args.emit_cpp and not args.detect_handles:
        print("--emit-cpp имеет смысл только с --detect-handles", file=sys.stderr)
        return 2

    filter_handles: set[int] | None = None
    if args.handles:
        filter_handles = set()
        for h in args.handles:
            parsed = _normalize_handle(h)
            if parsed is None:
                parsed = _hex_parse(h)
            if parsed is not None:
                filter_handles.add(parsed)

    try:
        import pyshark
    except ImportError:
        print("Нужен pyshark: pip install -r tools/requirements-btsnoop.txt", file=sys.stderr)
        return 1

    if args.detect_handles:
        try:
            res = run_detect_handles(args.capture, pyshark, args.max)
        except Exception as e:
            print(f"Ошибка: {e}", file=sys.stderr)
            return 1
        if res["scanned"] == 0:
            print(
                "ATT-пакетов нет (btatt пуст в этом файле). Нужен snoop с расшифрованным ATT "
                "или другой фрагмент захвата.",
                file=sys.stderr,
            )
            return 2
        print_detect_handles_report(res, args.emit_cpp)
        return 0

    if args.summary:
        try:
            st = run_summary(args.capture, pyshark, args.max)
        except Exception as e:
            print(f"Ошибка: {e}", file=sys.stderr)
            return 1
        print("=== summary ===")
        for k, v in st.items():
            print(f"  {k}: {v}")
        if st["packets"] and st["btatt"] == 0:
            print(
                "\nПримечание: btatt=0 при наличии ACL/L2CAP часто значит шифрование LL "
                "или отсутствие ключей в Wireshark; попробуйте --l2cap.",
                file=sys.stderr,
            )
        return 0

    if args.l2cap:
        try:
            scanned, rows = run_l2cap_dump(
                args.capture,
                pyshark,
                max_packets=args.max,
                grep_hex=args.grep_hex,
            )
        except Exception as e:
            print(f"Ошибка: {e}", file=sys.stderr)
            return 1
        print(f"\n# packets scanned: {scanned}, L2CAP rows: {rows}", file=sys.stderr)
        return 0

    df = None if args.no_filter else (args.display_filter or None)

    try:
        if df:
            cap = pyshark.FileCapture(args.capture, display_filter=df, keep_packets=False)
        else:
            cap = pyshark.FileCapture(args.capture, keep_packets=False)
    except Exception as e:
        print(f"Не удалось открыть capture: {e}", file=sys.stderr)
        print("Убедитесь, что tshark установлен и доступен в PATH.", file=sys.stderr)
        return 1

    count = 0
    att_shown = 0
    try:
        for pkt in cap:
            count += 1
            if args.max and count > args.max:
                break
            if not hasattr(pkt, "btatt"):
                continue
            layer = pkt.btatt
            fields = _layer_fields(layer)

            if args.raw_fields:
                print(f"\n--- #{_pkt_no(pkt)} btatt fields ---")
                for k in sorted(fields):
                    print(f"  {k} = {fields[k]}")
                att_shown += 1
                continue

            opcode_n, opcode_raw = _pick_opcode(fields)
            handle = _pick_handle(fields)
            value_hex = _pick_value_hex(fields)

            if filter_handles is not None and handle is not None:
                if handle not in filter_handles:
                    continue

            op_name = ATT_OPCODE_NAMES.get(opcode_n, opcode_raw or "?")

            ts_s = _pkt_ts_iso(pkt)

            handle_s = f"0x{handle:04x}" if handle is not None else "?"
            op_s = f"0x{opcode_n:02x}" if opcode_n is not None else "?"

            line = (
                f"{_pkt_no(pkt):6d}  {ts_s:26s}  {op_s} {op_name:28s}  "
                f"handle={handle_s}  val_len={len(value_hex)//2}  {value_hex[:80]}"
            )
            print(line)
            if len(value_hex) > 80:
                print(f"{'':8}{value_hex[80:]}")
            att_shown += 1
    finally:
        cap.close()

    print(f"\n# packets scanned: {count}, ATT rows printed: {att_shown}", file=sys.stderr)
    if count > 0 and att_shown == 0 and not args.raw_fields:
        print(
            "Подсказка: ATT не декодирован (часто шифрование). Запустите:\n"
            f"  python3 tools/analyze_btsnoop.py {args.capture} --summary\n"
            f"  python3 tools/analyze_btsnoop.py {args.capture} --l2cap --max 30",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
