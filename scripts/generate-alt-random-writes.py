#!/usr/bin/env python3
import argparse
import base64
import enum
import fcntl
import random
import signal
import struct
import sys
import termios
import zlib
from typing import Any


class ColorVariant(enum.IntEnum):
    NONE = enum.auto()
    REGULAR = enum.auto()
    BRIGHT = enum.auto()
    CUBE = enum.auto()
    RGB = enum.auto()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        'out', type=argparse.FileType(mode='w'), nargs='?', help='name of output file')
    parser.add_argument('--cols', type=int)
    parser.add_argument('--rows', type=int)
    parser.add_argument('--colors-regular', action='store_true')
    parser.add_argument('--colors-bright', action='store_true')
    parser.add_argument('--colors-256', action='store_true')
    parser.add_argument('--colors-rgb', action='store_true')
    parser.add_argument('--scroll', action='store_true')
    parser.add_argument('--scroll-region', action='store_true')
    parser.add_argument('--attr-bold', action='store_true')
    parser.add_argument('--attr-italic', action='store_true')
    parser.add_argument('--attr-underline', action='store_true')
    parser.add_argument('--sixel', action='store_true')
    parser.add_argument('--kitty', action='store_true',
                        help='kitty graphics protocol images')
    parser.add_argument('--seed', type=int)

    opts = parser.parse_args()
    out = opts.out if opts.out is not None else sys.stdout

    lines: int | None = None
    cols: int | None = None
    width: int | None = None
    height: int | None = None

    if opts.rows is None or opts.cols is None:
        try:
            def dummy(*args: Any) -> None:
                """Need a handler installed for sigwait() to trigger."""
                _ = args
            signal.signal(signal.SIGWINCH, dummy)

            while True:
                with open('/dev/tty', 'rb') as pty:
                    lines, cols, height, width = struct.unpack(
                        'HHHH',
                        fcntl.ioctl(pty,
                                    termios.TIOCGWINSZ,
                                    struct.pack('HHHH', 0, 0, 0, 0)))

                assert width is not None
                assert height is not None

                if width > 0 and height > 0:
                    break

                # We’re early; the foot window hasn’t been mapped yet. Or,
                # to be more precise, fonts haven’t yet been loaded,
                # meaning it doesn’t have any cell geometry yet.
                signal.sigwait([signal.SIGWINCH])

            signal.signal(signal.SIGWINCH, signal.SIG_DFL)

        except OSError:
            lines = None
            cols = None
            height = None
            width = None

    if opts.rows is not None:
        lines = opts.rows
        assert lines is not None
        height = 15 * lines  # PGO helper binary hardcodes cell height to 15px
    if opts.cols is not None:
        cols = opts.cols
        assert cols is not None
        width = 8 * cols     # PGO help binary hardcodes cell width to 8px

    if lines is None or cols is None or height is None or width is None:
        raise ValueError('could not get terminal width/height; use --rows and --cols')

    assert lines > 0, f'{lines}'
    assert cols > 0, f'{cols}'
    assert width > 0, f'{width}'
    assert height > 0, f'{height}'

    # Number of characters to write to screen
    count = 256 * 1024**1

    # Characters to choose from
    alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRTSTUVWXYZ0123456789 öäå 👨👩🧒👩🏽‍🔬🇸🇪'

    color_variants = ([ColorVariant.NONE] +
                      ([ColorVariant.REGULAR] if opts.colors_regular else []) +
                      ([ColorVariant.BRIGHT] if opts.colors_bright else []) +
                      ([ColorVariant.CUBE] if opts.colors_256 else []) +
                      ([ColorVariant.RGB] if opts.colors_rgb else []))

    # Enter alt screen
    out.write('\033[?1049h')

    # uses system time or /dev/urandom if available if opt.seed == None
    # pin seeding method to make seeding stable across future versions
    random.seed(a=opts.seed, version=2)

    for _ in range(count):
        if opts.scroll and random.randrange(256) == 0:
            out.write('\033[m')

            if opts.scroll_region and random.randrange(256) == 0:
                top = random.randrange(3)
                bottom = random.randrange(3)
                out.write(f'\033[{top};{lines - bottom}r')

            lines_to_scroll = random.randrange(lines - 1)
            rev = random.randrange(2)
            if not rev and random.randrange(2):
                out.write(f'\033[{lines};{cols}H')
                out.write('\n' * lines_to_scroll)
            else:
                out.write(f'\033[{lines_to_scroll + 1}{"T" if rev == 1 else "S"}')
            continue

        # Generate a random location and a random character
        row = random.randrange(lines)
        col = random.randrange(cols)
        c = random.choice(alphabet)

        repeat = random.randrange((cols - col) + 1)
        assert col + repeat <= cols

        color_variant = random.choice(color_variants)

        # Position cursor
        out.write(f'\033[{row + 1};{col + 1}H')

        if color_variant in [ColorVariant.REGULAR, ColorVariant.BRIGHT]:
            do_bg = random.randrange(2)
            base = 40 if do_bg else 30
            base += 60 if color_variant == ColorVariant.BRIGHT else 0

            idx = random.randrange(8)
            out.write(f'\033[{base + idx}m')

        elif color_variant == ColorVariant.CUBE:
            do_bg = random.randrange(2)
            base = 48 if do_bg else 38

            idx = random.randrange(256)
            if random.randrange(2):
                # Old-style
                out.write(f'\033[{base};5;{idx}m')
            else:
                # New-style (sub-parameter based)
                out.write(f'\033[{base}:5:{idx}m')

        elif color_variant == ColorVariant.RGB:
            do_bg = random.randrange(2)
            base = 48 if do_bg else 38

            # use list comprehension in favor of randbytes(n)
            # which is only available for Python >= 3.9
            rgb = [random.randrange(256) for _ in range(3)]

            if random.randrange(2):
                # Old-style
                out.write(f'\033[{base};2;{rgb[0]};{rgb[1]};{rgb[2]}m')
            else:
                # New-style (sub-parameter based)
                out.write(f'\033[{base}:2::{rgb[0]}:{rgb[1]}:{rgb[2]}m')

        if opts.attr_bold and random.randrange(5) == 0:
            out.write('\033[1m')
        if opts.attr_italic and random.randrange(5) == 0:
            out.write('\033[3m')
        if opts.attr_underline and random.randrange(5) == 0:
            out.write('\033[4m')

        out.write(c * repeat)

        do_sgr_reset = random.randrange(2)
        if do_sgr_reset:
            reset_actions = ['\033[m', '\033[39m', '\033[49m']
            out.write(random.choice(reset_actions))

    # Reset colors
    out.write('\033[m\033[r')

    if opts.sixel:
        # The sixel 'alphabet'
        sixels = '?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~'

        last_pos: tuple[int, int] | None = None
        last_size: tuple[int, int] = 0, 0

        for _ in range(20):
            if last_pos is not None and random.randrange(2):
                # Overwrite last sixel. I.e. use same position and
                # size as last sixel
                pass
            else:
                # Random origin in upper left quadrant
                last_pos = random.randrange(lines // 2) + 1, random.randrange(cols // 2) + 1
                last_size = random.randrange((height + 1) // 2), random.randrange((width + 1) // 2)

            out.write(f'\033[{last_pos[0]};{last_pos[1]}H')
            six_height, six_width = last_size
            six_rows = (six_height + 5) // 6  # Round up; each sixel is 6 pixels

            # Begin sixel (with P2 set to either 0 or 1 - opaque or transparent)
            sixel_p2 = random.randrange(2)
            out.write(f'\033P;{sixel_p2}q')

            # Sixel size. Without this, sixels will be
            # auto-resized on cell-boundaries.
            out.write(f'"1;1;{six_width};{six_height}')

            # Set up 256 random colors
            for idx in range(256):
                # param 2: 1=HLS, 2=RGB.
                # param 3/4/5: HLS/RGB values in range 0-100
                #              (except 'hue' which is 0..360)
                out.write(f'#{idx};2;{random.randrange(101)};{random.randrange(101)};{random.randrange(101)}')

            for row in range(six_rows):
                band_count = random.randrange(4, 33)
                for band in range(band_count):
                    # Choose a random color
                    out.write(f'#{random.randrange(256)}')

                    if random.randrange(2):
                        for col in range(six_width):
                            out.write(f'{random.choice(sixels)}')
                    else:
                        pix_left = six_width
                        while pix_left > 0:
                            repeat_count = random.randrange(1, pix_left + 1)
                            out.write(f'!{repeat_count}{random.choice(sixels)}')
                            pix_left -= repeat_count

                    # Next line
                    if band + 1 < band_count:
                        # Move cursor to beginning of current row
                        out.write('$')
                    elif row + 1 < six_rows:
                        # Newline
                        out.write('-')

            # End sixel
            out.write('\033\\')

    if opts.kitty:
        emit_kitty_graphics(out, lines, cols, width, height)

    # Leave alt screen
    out.write('\033[?1049l')


def kitty_apc(out: Any, control: str, payload: bytes = b'', chunk: int = 4096) -> None:
    """Emit a kitty graphics command, chunking the base64 payload."""
    data = base64.standard_b64encode(payload).decode('ascii')
    if len(data) <= chunk:
        out.write(f'\033_G{control};{data}\033\\' if data else f'\033_G{control}\033\\')
        return

    for i in range(0, len(data), chunk):
        more = 1 if i + chunk < len(data) else 0
        ctrl = f'{control},m={more}' if i == 0 else f'm={more}'
        out.write(f'\033_G{ctrl};{data[i:i + chunk]}\033\\')


def emit_kitty_graphics(out: Any, lines: int, cols: int, width: int, height: int) -> None:
    """Exercise the kitty graphics protocol: transmission (raw, zlib,
    chunked), placements above/below text, scrolling, animation
    frames, unicode placeholders and deletion."""
    cell_w = max(width // cols, 1)
    cell_h = max(height // lines, 1)

    def random_image(w: int, h: int, alpha: bool) -> bytes:
        bpp = 4 if alpha else 3
        # A few random colors in horizontal bands; cheap to generate,
        # while still compressing reasonably
        rows = []
        for _ in range(h):
            px = bytes(random.randrange(256) for _ in range(bpp))
            rows.append(px * w)
        return b''.join(rows)

    image_ids = []

    for i in range(24):
        img_id = i + 1
        w = random.randrange(1, max(width // 3, 2))
        h = random.randrange(1, max(height // 3, 2))
        alpha = bool(random.randrange(2))
        data = random_image(w, h, alpha)
        fmt = 32 if alpha else 24

        control = f'a=t,i={img_id},f={fmt},s={w},v={h},q=2'
        if random.randrange(2):
            data = zlib.compress(data)
            control += ',o=z'

        kitty_apc(out, control, data)
        image_ids.append(img_id)

    # Placements: some below the text, some above, some scaled
    for _ in range(64):
        out.write(f'\033[{random.randrange(lines) + 1};{random.randrange(cols) + 1}H')

        img_id = random.choice(image_ids)
        z = random.choice([-1073741825, -1, 0, 1])
        control = f'a=p,i={img_id},z={z},q=2'

        if random.randrange(2):
            control += f',c={random.randrange(1, max(cols // 4, 2))}'
        if random.randrange(2):
            control += f',r={random.randrange(1, max(lines // 4, 2))}'
        if random.randrange(3) == 0:
            control += f',X={random.randrange(cell_w)},Y={random.randrange(cell_h)}'
        if random.randrange(3) == 0:
            control += ',C=1'
        if random.randrange(4) == 0:
            control += f',p={random.randrange(1, 4)}'

        kitty_apc(out, control)

        # Text on top of, and around, the images
        out.write(f'\033[3{random.randrange(8)}m')
        out.write(''.join(chr(random.randrange(0x21, 0x7f)) for _ in range(random.randrange(cols))))

    # Scroll the images, including with a scroll region
    out.write('\033[m')
    for _ in range(lines // 2):
        out.write('\n' * random.randrange(1, 4))
        out.write(f'\033[{random.randrange(lines) + 1};1H')
        out.write(''.join(chr(random.randrange(0x21, 0x7f)) for _ in range(cols // 2)))

    out.write(f'\033[2;{max(lines - 1, 3)}r')
    for _ in range(lines // 2):
        out.write(f'\033[{max(lines - 1, 3)};1H\n')
    out.write('\033[r')

    # Animation frames
    w, h = max(width // 8, 1), max(height // 8, 1)
    kitty_apc(out, f'a=T,i=1000,f=24,s={w},v={h},q=2', random_image(w, h, False))
    for _ in range(8):
        fw = random.randrange(1, w + 1)
        fh = random.randrange(1, h + 1)
        kitty_apc(
            out,
            f'a=f,i=1000,f=32,x={random.randrange(w - fw + 1)},y={random.randrange(h - fh + 1)},'
            f's={fw},v={fh},c={random.randrange(1, 3)},z={random.randrange(10, 100)},q=2',
            random_image(fw, fh, True))
    kitty_apc(out, f'a=c,i=1000,r=2,c=3,w={w},h={h},q=2')
    for frame in range(1, 10):
        kitty_apc(out, f'a=a,i=1000,c={frame},q=2')

    # Unicode placeholders
    w, h = 8 * cell_w, 3 * cell_h
    kitty_apc(out, f'a=T,U=1,i=42,f=24,s={w},v={h},c=8,r=3,q=2', random_image(w, h, False))
    diacritics = ['\u0305', '\u030d', '\u030e']
    out.write('\033[1;1H\033[38;5;42m')
    for row in range(3):
        out.write(f'\U0010EEEE{diacritics[row]}' + '\U0010EEEE' * 7 + '\r\n')
    out.write('\033[39m')

    # Deletion
    for d in 'aipcxyzr':
        x = random.randrange(cols) + 1
        y = random.randrange(lines) + 1
        img_id = random.choice(image_ids)
        kitty_apc(out, f'a=d,d={d},i={img_id},x={x},y={y},z=-1,q=2')

    for img_id in image_ids:
        kitty_apc(out, f'a=d,d=I,i={img_id},q=2')
    kitty_apc(out, 'a=d,d=A,q=2')


if __name__ == '__main__':
    main()
