import argparse
import dataclasses
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

SCRIPT_NAME = "Atmos Audio Decoder"
SCRIPT_VERSION = "0.1.0"

if platform.system() != "Darwin":
    raise RuntimeError("Error: This script is designed to run on macOS only.")


@dataclasses.dataclass
class Config:
    mode: str
    gst_exe_path: pathlib.Path
    ffmpeg_exe_path: pathlib.Path
    atmos_fps: float
    atmos_tool_name: str
    atmos_tool_version: str


class AtmosDecode:
    def __init__(self, config: Config) -> None:
        self.config: Config = config
        self.custom_env = os.environ.copy()
        self.gst_exe_path = self._resolve_executable(
            self.config.gst_exe_path, "GStreamer"
        )
        self.ffmpeg_exe_path = self._resolve_executable(
            self.config.ffmpeg_exe_path, "ffmpeg"
        )
        self._gstreamer_env_ready = False

    def _ensure_gstreamer_env(self) -> None:
        if self._gstreamer_env_ready:
            return
        self._setup_gstreamer_env()
        self._gstreamer_env_ready = True

    @staticmethod
    def _resolve_executable(path: pathlib.Path, name: str) -> pathlib.Path:
        resolved = shutil.which(str(path))
        if resolved:
            return pathlib.Path(resolved).resolve()
        direct_path = pathlib.Path(path).expanduser().resolve()
        if direct_path.is_file():
            return direct_path
        raise RuntimeError(
            f"{name} executable '{path}' not found in PATH or as a direct file path."
        )

    def _find_custom_plugin_dir(self) -> pathlib.Path | None:
        candidates = []
        env_path = os.environ.get("OAMD_PLUGIN_PATH")
        if env_path:
            candidates.append(pathlib.Path(env_path).expanduser())
        script_dir = pathlib.Path(__file__).resolve().parent
        candidates.append(
            script_dir / "build" / "cmake-x86_64" / "reference-player-plugins"
        )
        candidates.append(
            pathlib.Path.cwd()
            / "build"
            / "cmake-x86_64"
            / "reference-player-plugins"
        )
        for candidate in candidates:
            if candidate.is_dir():
                return candidate.resolve()
        return None

    def _setup_gstreamer_env(self) -> None:
        plugin_paths: list[str] = []

        def add_plugin_path(path: pathlib.Path | None) -> None:
            if path is None or not path.is_dir():
                return
            resolved = str(path.resolve())
            if resolved not in plugin_paths:
                plugin_paths.append(resolved)

        custom_plugin_dir = self._find_custom_plugin_dir()
        add_plugin_path(custom_plugin_dir)

        existing_plugin_path = self.custom_env.get("GST_PLUGIN_PATH")
        if existing_plugin_path:
            for part in existing_plugin_path.split(os.pathsep):
                if part:
                    add_plugin_path(pathlib.Path(part))

        app_root = None
        for parent_dir in self.gst_exe_path.parents:
            if parent_dir.name.endswith(".app"):
                app_root = parent_dir
                break

        if app_root is not None:
            resources_dir = (
                app_root
                / "Contents"
                / "Frameworks"
                / "GStreamer.framework"
                / "Versions"
                / "Current"
                / "Resources"
            )
            add_plugin_path(resources_dir / "lib" / "gstreamer-1.0")
            add_plugin_path(app_root / "Contents" / "PlugIns" / "gst-plugins")

            scanner_path = resources_dir / "libexec" / "gst-plugin-scanner"
            if scanner_path.is_file() and os.access(scanner_path, os.X_OK):
                self.custom_env["GST_PLUGIN_SCANNER"] = str(
                    scanner_path.resolve()
                )
            else:
                print(
                    f"Warning: gst-plugin-scanner not found at {scanner_path}.",
                    file=sys.stderr,
                )

            if custom_plugin_dir is None:
                print(
                    "Warning: custom gst-plugin-oamd build not found; "
                    "using the Dolby Reference Player bundled plugin.",
                    file=sys.stderr,
                )
        else:
            print(
                "Info: Using default GStreamer plugin discovery. "
                "Set GST_PLUGIN_PATH/GST_PLUGIN_SCANNER manually if needed."
            )

        if plugin_paths:
            self.custom_env["GST_PLUGIN_PATH"] = os.pathsep.join(plugin_paths)
            print(f"Info: GST_PLUGIN_PATH={self.custom_env['GST_PLUGIN_PATH']}")

        if custom_plugin_dir is not None:
            print(f"Info: Using custom plugin directory: {custom_plugin_dir}")

    @staticmethod
    def _prepare_file_path_for_cli(source_path: pathlib.Path) -> str:
        return str(source_path.expanduser().resolve())

    @staticmethod
    def _read_header_kind(path: pathlib.Path) -> str | None:
        with path.open("rb") as file:
            header = file.read(4096)
        if header.startswith(b"\x0b\x77"):
            return "eac3"
        if b"\xf8\x72\x6f\xba" in header:
            return "truehd"
        return None

    def _detect_codec(self, input_file: pathlib.Path) -> str:
        codec = self._read_header_kind(input_file)
        if codec == "eac3":
            print("Info: Detected E-AC-3 input.")
            return codec
        if codec == "truehd":
            print("Info: Detected TrueHD input.")
            return codec
        raise RuntimeError(
            "Input is not a raw E-AC-3 or TrueHD stream. "
            "Demux the container first and pass a .eac3/.ec3 or .mlp/.thd file."
        )

    def _decoder_args(self, codec: str) -> list[str]:
        if codec == "eac3":
            return [
                "dlbac3parse",
                "enable-metadata=true",
                "!",
                "dlbac3dec",
                "out-ch-config=21",
                "!",
            ]
        if codec == "truehd":
            return [
                "dlbtruehdparse",
                "align-major-sync=false",
                "!",
                "dlbtruehddec",
                "out-ch-config=21",
                "presentation=16",
                "!",
            ]
        raise RuntimeError(f"Unsupported codec: {codec}")

    def _build_decode_cmd(
        self,
        input_file: pathlib.Path,
        codec: str,
        out_pcm_file: pathlib.Path,
        out_metadata_file: pathlib.Path,
    ) -> list[str]:
        command = [
            str(self.gst_exe_path),
            "filesrc",
            f"location={self._prepare_file_path_for_cli(input_file)}",
            "!",
        ]
        command.extend(self._decoder_args(codec))
        command.extend(
            [
                "tee",
                "name=t",
                "!",
                "queue",
                "!",
                "oamdcapsfeature",
                "remove=true",
                "!",
                "audioconvert",
                "!",
                "audio/x-raw,format=S24LE,layout=interleaved",
                "!",
                "filesink",
                f"location={self._prepare_file_path_for_cli(out_pcm_file)}",
                "t.",
                "!",
                "queue",
                "!",
                "oamdserialize",
                "!",
                "filesink",
                f"location={self._prepare_file_path_for_cli(out_metadata_file)}",
            ]
        )
        return command

    def _build_binaural_cmd(
        self,
        input_file: pathlib.Path,
        codec: str,
        out_file: pathlib.Path,
    ) -> list[str]:
        command = [
            str(self.gst_exe_path),
            "filesrc",
            f"location={self._prepare_file_path_for_cli(input_file)}",
            "!",
        ]
        command.extend(self._decoder_args(codec))
        command.extend(
            [
                "oamdcapsfeature",
                "float=true",
                "!",
                "dlbdapvr",
                "!",
                "wavenc",
                "!",
                "filesink",
                f"location={self._prepare_file_path_for_cli(out_file)}",
            ]
        )
        return command

    @staticmethod
    def _read_metadata_ids(metadata_file: pathlib.Path) -> list[int]:
        ids: set[int] = set()
        pattern = re.compile(r"^\s*-\s*ID:\s*(\d+)\s*$")
        with metadata_file.open("r", encoding="utf-8") as file:
            for line in file:
                match = pattern.match(line)
                if match:
                    ids.add(int(match.group(1)))
        return sorted(ids)

    def _generate_atmos_file_content(
        self, metadata_filename: str, caf_filename: str, object_ids: list[int]
    ) -> str:
        object_ids_str = "\n".join([f"      - ID: {i}" for i in object_ids])

        return f"""
version: 0.5.1
presentations:
  - type: home
    simplified: false
    metadata: {metadata_filename}
    audio: {caf_filename}
    offset: 0
    fps: {self.config.atmos_fps}
    scBedConfiguration: [3]
    creationTool: {self.config.atmos_tool_name}
    creationToolVersion: {self.config.atmos_tool_version}
    downmixType_5to2: LoRo_Stereo
    51-to-20_LsRs90degPhaseShift: false
    warpMode: warping
    bedInstances:
      - channels:
          - channel: LFE
            ID: 3
    objects:
{object_ids_str}
"""

    def _resolve_output_paths(
        self,
        input_file: pathlib.Path,
        output_base_path: pathlib.Path | None,
    ) -> tuple[pathlib.Path, str]:
        if output_base_path is None:
            return input_file.parent, input_file.stem
        output_base_path = output_base_path.expanduser()
        if output_base_path.is_dir():
            return output_base_path, input_file.stem
        return output_base_path.parent, output_base_path.stem

    def decode(
        self,
        input_file: pathlib.Path,
        output_base_path: pathlib.Path | None = None,
    ) -> None:
        input_file = input_file.expanduser()
        if not input_file.is_file():
            raise RuntimeError(
                f"Input file {input_file.resolve()} is not a regular file."
            )

        codec = self._detect_codec(input_file)
        self._ensure_gstreamer_env()
        out_dir, out_fname_base = self._resolve_output_paths(
            input_file, output_base_path
        )
        out_dir.mkdir(parents=True, exist_ok=True)

        out_pcm_file = out_dir / f"{out_fname_base}.pcm"
        out_metadata_file = out_dir / f"{out_fname_base}.metadata"
        out_caf_file = out_dir / f"{out_fname_base}.caf"
        out_atmos_file = out_dir / f"{out_fname_base}.atmos"

        print(f'Info: Decoding "{input_file.name}" to PCM and Atmos metadata...')
        gst_cmd = self._build_decode_cmd(
            input_file, codec, out_pcm_file, out_metadata_file
        )
        subprocess.run(gst_cmd, check=True, env=self.custom_env)
        if not out_metadata_file.is_file() or out_metadata_file.stat().st_size == 0:
            raise RuntimeError(
                "No OAMD metadata was produced; "
                "the input may not be Dolby Atmos."
            )
        if not out_pcm_file.is_file() or out_pcm_file.stat().st_size == 0:
            raise RuntimeError("The decoded PCM output is empty.")
        print(f"  PCM output:      {out_pcm_file.name}")
        print(f"  Metadata output: {out_metadata_file.name}")

        print(
            f"Info: Converting PCM '{out_pcm_file.name}' "
            f"to CAF '{out_caf_file.name}'..."
        )
        ffmpeg_cmd = [
            str(self.ffmpeg_exe_path),
            "-y",
            "-v",
            "error",
            "-f",
            "s24le",
            "-ar",
            "48k",
            "-ac",
            "16",
            "-i",
            str(out_pcm_file),
            "-c:a",
            "copy",
            str(out_caf_file),
        ]
        result = subprocess.run(ffmpeg_cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(
                f"ffmpeg failed to create CAF:\n{result.stderr.strip()}"
            )
        print(f"  CAF output:      {out_caf_file.name}")

        print(f"Info: Generating .atmos file '{out_atmos_file.name}'...")
        metadata_ids = self._read_metadata_ids(out_metadata_file)
        object_ids = sorted(
            set(range(10, 25))
            | {object_id for object_id in metadata_ids if object_id >= 10}
        )
        bed_ids = [object_id for object_id in metadata_ids if object_id < 10]
        extra_bed_ids = [bed_id for bed_id in bed_ids if bed_id != 3]
        if extra_bed_ids:
            print(
                "Warning: Metadata contains additional bed IDs "
                f"{extra_bed_ids}; the .atmos template only declares the "
                "LFE bed.",
                file=sys.stderr,
            )
        atmos_content = self._generate_atmos_file_content(
            out_metadata_file.name, out_caf_file.name, object_ids
        )
        with open(out_atmos_file, "w", encoding="utf-8") as file:
            file.write(atmos_content)
        print(f"  .atmos output:   {out_atmos_file.name}")
        print(
            "Info: Decode complete. Outputs: "
            ".atmos, .metadata, .caf, and .pcm."
        )

    def binaural(
        self,
        input_file: pathlib.Path,
        output_base_path: pathlib.Path | None = None,
    ) -> None:
        input_file = input_file.expanduser()
        if not input_file.is_file():
            raise RuntimeError(
                f"Input file {input_file.resolve()} is not a regular file."
            )

        codec = self._detect_codec(input_file)
        self._ensure_gstreamer_env()
        if output_base_path is None:
            out_fpath = input_file.with_suffix(".binaural.wav")
        else:
            output_base_path = output_base_path.expanduser()
            if output_base_path.is_dir():
                out_fpath = output_base_path / f"{input_file.stem}.binaural.wav"
            else:
                out_fpath = output_base_path.with_suffix(".wav")
        out_fpath.parent.mkdir(parents=True, exist_ok=True)

        print(f'Info: Rendering binaural audio to "{out_fpath.name}"...')
        gst_cmd = self._build_binaural_cmd(input_file, codec, out_fpath)
        subprocess.run(gst_cmd, check=True, env=self.custom_env)
        if not out_fpath.is_file() or out_fpath.stat().st_size == 0:
            raise RuntimeError("Binaural rendering produced no output.")
        print("Info: Binaural rendering completed.")

def main() -> None:
    available_modes = ["decode", "binaural"]

    parser = argparse.ArgumentParser(
        description=f"{SCRIPT_NAME} v{SCRIPT_VERSION} - Decodes Dolby Atmos audio files on macOS.",
        formatter_class=argparse.RawTextHelpFormatter,
    )
    parser.add_argument(
        "-i",
        "--input",
        help="Path to a demuxed raw audio stream: E-AC-3 (.eac3/.ec3) "
        "or TrueHD (.mlp/.thd). Container files are not accepted.",
        type=pathlib.Path,
        required=True,
    )
    parser.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        help="Base path/name for output file(s).\n"
        '- "decode" mode:   Base for ".atmos", ".metadata", ".caf", ".pcm". Default: beside input.\n'
        '- "binaural" mode: Full path for binaural ".wav" output. Default: "input.binaural.wav".',
    )
    parser.add_argument(
        "--mode",
        default="decode",
        choices=available_modes,
        type=str,
        help="Processing mode:\n"
        "  decode:   Decode to .atmos, .metadata, .caf, and .pcm files (GStreamer + ffmpeg).\n"
        "  binaural: Render a binaural WAV (GStreamer).",
    )

    gst_group = parser.add_argument_group("GStreamer options")
    gst_group.add_argument(
        "--gst_exe_path",
        help="Path to 'gst-launch-1.0'. Default: "
        "'/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/bin/gst-launch-1.0'.",
        type=pathlib.Path,
        default=pathlib.Path(
            "/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/bin/gst-launch-1.0"
        ),
    )

    common_group = parser.add_argument_group("Common options")
    common_group.add_argument(
        "--ffmpeg_path",
        type=pathlib.Path,
        default=pathlib.Path("ffmpeg"),
        help="Path to 'ffmpeg', used for CAF conversion in decode mode. "
        "Default: 'ffmpeg' from PATH.",
    )

    atmos_files_group = parser.add_argument_group('Options for "decode" mode')
    atmos_files_group.add_argument(
        "--atmos_fps",
        type=float,
        default=24.0,
        help="FPS for .atmos file. Default: 24.0.",
    )
    atmos_files_group.add_argument(
        "--atmos_tool_name",
        type=str,
        default="YUYGFGG",
        help="Creation tool name for .atmos file. Default: YUYGFGG.",
    )
    atmos_files_group.add_argument(
        "--atmos_tool_version",
        type=str,
        default=SCRIPT_VERSION,
        help=f"Creation tool version for .atmos file. Default: {SCRIPT_VERSION}.",
    )

    args = parser.parse_args()

    script_config = Config(
        mode=args.mode,
        gst_exe_path=args.gst_exe_path,
        ffmpeg_exe_path=args.ffmpeg_path,
        atmos_fps=args.atmos_fps,
        atmos_tool_name=args.atmos_tool_name,
        atmos_tool_version=args.atmos_tool_version,
    )

    try:
        decoder = AtmosDecode(script_config)
        if args.mode == "decode":
            decoder.decode(args.input, args.output)
        elif args.mode == "binaural":
            decoder.binaural(args.input, args.output)
        else:
            raise RuntimeError(f"Unknown processing mode: {args.mode}")
        print(f"Info: Processing via mode '{args.mode}' completed successfully.")
    except subprocess.CalledProcessError as error:
        command_str = (
            " ".join(map(str, error.cmd))
            if isinstance(error.cmd, list)
            else str(error.cmd)
        )
        stdout = (
            f"Stdout:\n{error.stdout.decode(errors='replace') if isinstance(error.stdout, bytes) else str(error.stdout)}"
            if error.stdout
            else "Stdout: (Empty)"
        )
        stderr = (
            f"Stderr:\n{error.stderr.decode(errors='replace') if isinstance(error.stderr, bytes) else str(error.stderr)}"
            if error.stderr
            else "Stderr: (Empty)"
        )
        print(
            f"Error: External command execution failed: {command_str}\n"
            f"Return code: {error.returncode}\n"
            f"{stdout}\n"
            f"{stderr}",
            file=sys.stderr,
        )
        sys.exit(1)
    except RuntimeError as error:
        print(f"Error: {error}", file=sys.stderr)
        sys.exit(1)
    except Exception as error:
        print(f"Error: An unexpected error occurred: {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
