#!/usr/bin/env vpython3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Python CLI for flashing ChromiumOS EC firmware over Servo."""

import argparse
import logging
import os
import subprocess
import sys
from typing import List, Optional

from flash.factory import FlasherFactory
from flash.servo_utils import ServoDriver
from flash.servo_utils import ServoError


def setup_logging(verbose: bool) -> None:
    """Configure root logger for the flash utility."""
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(levelname)s: %(message)s",
    )


def _add_bool_arg(
    parser: argparse.ArgumentParser,
    name: str,
    *,
    default: bool,
    help_text: str,
    short: Optional[str] = None,
) -> None:
    """Register a shflags-compatible boolean flag and its negated aliases."""
    hyphen = name.replace("_", "-")
    pos_flags = list(dict.fromkeys([f"--{name}", f"--{hyphen}"]))
    if short:
        pos_flags.append(short)
    parser.add_argument(
        *pos_flags,
        action="store_true",
        dest=name,
        default=default,
        help=help_text,
    )

    compact = name.replace("_", "")
    neg_flags = list(
        dict.fromkeys(
            [
                f"--no_{name}",
                f"--no{name}",
                f"--no-{hyphen}",
                f"--no{compact}",
            ]
        )
    )
    parser.add_argument(
        *neg_flags,
        action="store_false",
        dest=name,
        help=argparse.SUPPRESS,
    )


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(
        description="Flash EC firmware via Servo.",
        allow_abbrev=False,
    )

    parser.add_argument(
        "--board",
        "-b",
        type=str,
        default=os.environ.get("BOARD", ""),
        help="Target EC board name (defaults to $BOARD)",
    )
    parser.add_argument(
        "--image",
        "-i",
        type=str,
        help="Path to the firmware image file to write (required unless "
        "--read is given)",
    )
    parser.add_argument(
        "--read",
        "-r",
        type=str,
        help="Read current EC firmware into the specified file path",
    )
    parser.add_argument(
        "--port",
        "-p",
        type=int,
        default=os.environ.get("SERVOD_PORT") or "9999",
        help="Servod port (default: $SERVOD_PORT, else 9999)",
    )
    parser.add_argument(
        "--host",
        type=str,
        default="localhost",
        help="Servod host address (default: localhost)",
    )
    parser.add_argument(
        "--chip",
        "-c",
        type=str,
        help="Override the target EC chip type (e.g., it83xx, npcx_uut, stm32)",
    )
    parser.add_argument(
        "--interface",
        type=str,
        choices=["spi", "i2c", "uart", "dfu", "jtag"],
        help="Explicitly force a specific programming transport interface",
    )
    parser.add_argument(
        "--ec_source",
        type=str,
        default=os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "..")
        ),
        help="Path to the EC source tree root for dynamic board lookups",
    )

    _add_bool_arg(
        parser,
        "verify",
        default=True,
        help_text="Verify firmware after writing (default: True)",
    )
    parser.add_argument(
        "--no_preserve",
        "--nopreserve",
        "--no-preserve",
        action="store_true",
        dest="no_preserve",
        default=False,
        help="Do not preserve FMAP sections across flashes",
    )
    parser.add_argument(
        "--preserve",
        "--nono_preserve",
        "--nonopreserve",
        "--nono-preserve",
        action="store_false",
        dest="no_preserve",
        help=argparse.SUPPRESS,
    )
    region = parser.add_mutually_exclusive_group()
    region.add_argument(
        "--ro",
        action="store_true",
        default=False,
        help="Flash only the RO firmware region",
    )
    region.add_argument(
        "--rw",
        action="store_true",
        default=False,
        help="Flash only the RW firmware region",
    )
    parser.add_argument(
        "--no_ro",
        "--noro",
        "--no-ro",
        action="store_false",
        dest="ro",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--no_rw",
        "--norw",
        "--no-rw",
        action="store_false",
        dest="rw",
        help=argparse.SUPPRESS,
    )
    _add_bool_arg(
        parser,
        "dry_run",
        default=False,
        short="-n",
        help_text="Print commands without mutating hardware states",
    )
    _add_bool_arg(
        parser,
        "verbose",
        default=False,
        short="-v",
        help_text="Enable verbose debug logging",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        help="Timeout in seconds for individual flash tool invocations",
    )

    _add_bool_arg(
        parser,
        "raiden",
        default=False,
        help_text="Use Raiden USB SPI interface directly instead of FTDI",
    )
    parser.add_argument(
        "--baud",
        type=int,
        help="Override serial baudrate for UART-based flashers",
    )
    parser.add_argument(
        "--offset",
        type=int,
        default=0,
        help="Byte offset for flashing",
    )
    _add_bool_arg(
        parser,
        "unprotect",
        default=False,
        help_text="Remove flash read/write protection before programming",
    )
    _add_bool_arg(
        parser,
        "bitbang",
        default=False,
        help_text="Enable UART bitbang mode",
    )
    _add_bool_arg(
        parser,
        "try_apshutdown",
        default=False,
        help_text=(
            "Attempt to run 'apshutdown' on the EC console before and after "
            "flashing"
        ),
    )
    parser.add_argument(
        "--try_apshutdown_count",
        type=int,
        default=16,
        help="Number of times to send 'apshutdown' if --try_apshutdown is set",
    )
    parser.add_argument(
        "--mchp_loader",
        type=str,
        help="Path to the Microchip second_loader_fw.bin",
    )
    parser.add_argument(
        "--ocd_chip_cfg",
        type=str,
        help="OpenOCD target chip configuration filename",
    )
    parser.add_argument(
        "--ocd_cmds",
        type=str,
        help="Custom OpenOCD command sequence",
    )

    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    """Main entry point for the Python EC flasher."""
    args = parse_args(argv)
    setup_logging(args.verbose)

    try:
        if not args.read and not args.image:
            raise ValueError("--image is required unless --read is given")

        need_servo = True
        if args.chip == "stm32_dfu" or args.interface == "dfu":
            need_servo = False

        servo = None
        if need_servo:
            servo = ServoDriver(
                host=args.host, port=args.port, dry_run=args.dry_run
            )

        flasher = FlasherFactory.create(servo, args)
        flasher.run()
        return 0
    except (
        ServoError,
        RuntimeError,
        OSError,
        ValueError,
        NotImplementedError,
        subprocess.SubprocessError,
    ) as err:
        logging.error("Flash operation failed: %s", err)
        if args.verbose:
            logging.exception("Detailed traceback:")
        return 1


if __name__ == "__main__":
    sys.exit(main())
