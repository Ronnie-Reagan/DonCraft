from __future__ import annotations

import os
import queue
import re
import subprocess
import sys
import threading
from pathlib import Path

import build_DonCraft as cli


def should_launch_gui(argv: list[str]) -> bool:
    if "--gui" in argv:
        return True
    if "--cli" in argv:
        return False
    return len(argv) == 1


def run_cli() -> int:
    original_argv = sys.argv[:]
    sys.argv = [str(Path(cli.__file__).resolve()), *[arg for arg in sys.argv[1:] if arg not in {"--cli", "--gui"}]]
    try:
        return cli.main()
    finally:
        sys.argv = original_argv


def run_gui() -> int:
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext, ttk

    canonical_script = Path(cli.__file__).resolve()
    default_source_dir = canonical_script.parent

    root = tk.Tk()
    root.title("DonCraft Build / Dist")
    root.geometry("1040x700")
    root.minsize(900, 600)

    output_queue: queue.Queue[tuple[str, str | int]] = queue.Queue()
    process_holder: dict[str, subprocess.Popen[str] | None] = {"process": None}

    source_dir_var = tk.StringVar(value=str(default_source_dir))
    configure_preset_var = tk.StringVar(value="vs2022-release")
    build_preset_var = tk.StringVar(value="")
    test_preset_var = tk.StringVar(value="")
    config_var = tk.StringVar(value="Release")
    target_var = tk.StringVar(value="")
    package_mode_var = tk.StringVar(value=cli.DEFAULT_PACKAGE_MODE)
    update_channel_var = tk.StringVar(value=cli.DEFAULT_UPDATE_CHANNEL)
    update_branch_var = tk.StringVar(value=cli.DEFAULT_UPDATE_BRANCH)
    github_repo_var = tk.StringVar(value="")
    dist_dir_var = tk.StringVar(value=str(default_source_dir / "dist"))
    package_payload_dirs_var = tk.StringVar(value="")
    package_runtime_roots_var = tk.StringVar(value="")
    package_include_globs_var = tk.StringVar(value="")
    package_exclude_globs_var = tk.StringVar(value="")
    define_var = tk.StringVar(value="")
    run_label_var = tk.StringVar(value="")

    build_client_var = tk.BooleanVar(value=True)
    build_server_var = tk.BooleanVar(value=False)
    build_launcher_var = tk.BooleanVar(value=True)
    fresh_var = tk.BooleanVar(value=False)
    reconfigure_var = tk.BooleanVar(value=False)
    clean_first_var = tk.BooleanVar(value=False)
    ctest_var = tk.BooleanVar(value=False)
    no_package_var = tk.BooleanVar(value=False)
    no_dist_var = tk.BooleanVar(value=False)
    include_steam_appid_var = tk.BooleanVar(value=False)
    bootstrap_runtime_var = tk.BooleanVar(value=False)
    verbose_package_var = tk.BooleanVar(value=False)
    no_metrics_var = tk.BooleanVar(value=False)

    status_var = tk.StringVar(value="Ready")
    command_preview_var = tk.StringVar(value="")

    def browse_directory(variable: tk.StringVar, title: str) -> None:
        initial = variable.get().strip() or str(default_source_dir)
        selected = filedialog.askdirectory(title=title, initialdir=initial)
        if selected:
            variable.set(selected)
            update_command_preview()

    def add_repeated_args(command: list[str], option: str, raw_text: str) -> None:
        for part in re.split(r"[;\n]", raw_text):
            value = part.strip()
            if value:
                command.extend([option, value])

    def build_command() -> list[str]:
        command = [
            sys.executable,
            str(canonical_script),
            "--source-dir",
            source_dir_var.get().strip() or str(default_source_dir),
            "--configure-preset",
            configure_preset_var.get().strip() or "vs2022-release",
            "--config",
            config_var.get().strip() or "Release",
            "--package-mode",
            package_mode_var.get().strip() or cli.DEFAULT_PACKAGE_MODE,
            "--dist-dir",
            dist_dir_var.get().strip() or str(default_source_dir / "dist"),
            "--update-channel",
            update_channel_var.get().strip() or cli.DEFAULT_UPDATE_CHANNEL,
            "--update-branch",
            update_branch_var.get().strip() or cli.DEFAULT_UPDATE_BRANCH,
        ]

        optional_pairs = (
            (build_preset_var, "--build-preset"),
            (test_preset_var, "--test-preset"),
            (target_var, "--target"),
            (github_repo_var, "--github-repo"),
            (run_label_var, "--run-label"),
        )
        for variable, option in optional_pairs:
            value = variable.get().strip()
            if value:
                command.extend([option, value])

        command.append("--build-client" if build_client_var.get() else "--no-build-client")
        command.append("--build-server" if build_server_var.get() else "--no-build-server")
        command.append("--build-launcher" if build_launcher_var.get() else "--no-build-launcher")

        for enabled, option in (
            (fresh_var.get(), "--fresh"),
            (reconfigure_var.get(), "--reconfigure"),
            (clean_first_var.get(), "--clean-first"),
            (ctest_var.get(), "--ctest"),
            (no_package_var.get(), "--no-package"),
            (no_dist_var.get(), "--no-dist"),
            (include_steam_appid_var.get(), "--include-steam-appid"),
            (bootstrap_runtime_var.get(), "--launcher-bootstrap-runtime"),
            (verbose_package_var.get(), "--verbose-package"),
            (no_metrics_var.get(), "--no-metrics"),
        ):
            if enabled:
                command.append(option)

        add_repeated_args(command, "--package-payload-dir", package_payload_dirs_var.get())
        add_repeated_args(command, "--package-runtime-root", package_runtime_roots_var.get())
        add_repeated_args(command, "--package-include-glob", package_include_globs_var.get())
        add_repeated_args(command, "--package-exclude-glob", package_exclude_globs_var.get())
        add_repeated_args(command, "--define", define_var.get())
        return command

    def update_command_preview(*_ignored: object) -> None:
        try:
            command_preview_var.set(cli.stringify_command(build_command()))
        except Exception as error:
            command_preview_var.set(f"Could not build command preview: {error}")

    def append_output(text: str) -> None:
        output_box.configure(state="normal")
        output_box.insert("end", text)
        output_box.see("end")
        output_box.configure(state="disabled")

    def set_running(is_running: bool) -> None:
        start_button.configure(state="disabled" if is_running else "normal")
        stop_button.configure(state="normal" if is_running else "disabled")
        open_dist_button.configure(state="disabled" if is_running else "normal")
        copy_command_button.configure(state="disabled" if is_running else "normal")

    def run_worker(command: list[str], cwd: Path) -> None:
        try:
            creationflags = 0
            if os.name == "nt" and hasattr(subprocess, "CREATE_NEW_PROCESS_GROUP"):
                creationflags = subprocess.CREATE_NEW_PROCESS_GROUP

            process = subprocess.Popen(
                command,
                cwd=str(cwd),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                stdin=subprocess.DEVNULL,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
                creationflags=creationflags,
            )
            process_holder["process"] = process
            if process.stdout is not None:
                for line in process.stdout:
                    output_queue.put(("line", line))
            output_queue.put(("done", process.wait()))
        except Exception as error:
            output_queue.put(("line", f"\nGUI failed to start build process: {error}\n"))
            output_queue.put(("done", 1))

    def start_build() -> None:
        if process_holder["process"] is not None and process_holder["process"].poll() is None:
            messagebox.showwarning("Build already running", "A build is already running.")
            return

        source_dir = Path(source_dir_var.get().strip() or default_source_dir).expanduser().resolve()
        if not (source_dir / "CMakeLists.txt").exists():
            messagebox.showerror("Invalid source directory", f"CMakeLists.txt was not found in:\n{source_dir}")
            return

        command = build_command()
        output_box.configure(state="normal")
        output_box.delete("1.0", "end")
        output_box.configure(state="disabled")
        append_output("Starting DonCraft build/dist...\n")
        append_output(cli.stringify_command(command) + "\n\n")
        status_var.set("Running")
        set_running(True)
        threading.Thread(target=run_worker, args=(command, source_dir), daemon=True).start()

    def stop_build() -> None:
        process = process_holder["process"]
        if process is None or process.poll() is not None:
            return
        append_output("\nStopping build...\n")
        if os.name == "nt":
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        else:
            process.terminate()

    def poll_output() -> None:
        try:
            while True:
                kind, payload = output_queue.get_nowait()
                if kind == "line":
                    append_output(str(payload))
                elif kind == "done":
                    process_holder["process"] = None
                    returncode = int(payload)
                    status_var.set("Completed successfully" if returncode == 0 else f"Failed with exit code {returncode}")
                    append_output("\nBuild/dist completed successfully.\n" if returncode == 0 else f"\nBuild/dist failed with exit code {returncode}.\n")
                    set_running(False)
        except queue.Empty:
            pass
        root.after(80, poll_output)

    def open_dist_folder() -> None:
        path = Path(dist_dir_var.get().strip() or default_source_dir / "dist").expanduser().resolve()
        if not path.exists():
            messagebox.showinfo("Dist folder missing", f"The dist folder does not exist yet:\n{path}")
            return
        if os.name == "nt":
            os.startfile(path)  # type: ignore[attr-defined]
        elif sys.platform == "darwin":
            subprocess.Popen(["open", str(path)])
        else:
            subprocess.Popen(["xdg-open", str(path)])

    def copy_command() -> None:
        root.clipboard_clear()
        root.clipboard_append(command_preview_var.get())
        status_var.set("Command copied to clipboard")

    def make_labeled_entry(parent: tk.Widget, row: int, label: str, variable: tk.StringVar, *, width: int = 28, browse: bool = False) -> None:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=3)
        entry = ttk.Entry(parent, textvariable=variable, width=width)
        entry.grid(row=row, column=1, sticky="ew", pady=3)
        if browse:
            ttk.Button(parent, text="Browse", command=lambda: browse_directory(variable, label)).grid(row=row, column=2, sticky="ew", padx=(6, 0), pady=3)

    outer = ttk.Frame(root, padding=12)
    outer.pack(fill="both", expand=True)

    top = ttk.Frame(outer)
    top.pack(fill="x")

    left = ttk.LabelFrame(top, text="Build", padding=10)
    left.pack(side="left", fill="x", expand=True, padx=(0, 6))
    left.columnconfigure(1, weight=1)
    make_labeled_entry(left, 0, "Source dir", source_dir_var, width=46, browse=True)
    make_labeled_entry(left, 1, "Configure preset", configure_preset_var)
    make_labeled_entry(left, 2, "Build preset", build_preset_var)
    make_labeled_entry(left, 3, "Test preset", test_preset_var)
    make_labeled_entry(left, 4, "Config", config_var)
    make_labeled_entry(left, 5, "Target override", target_var)
    make_labeled_entry(left, 6, "Run label", run_label_var)

    right = ttk.LabelFrame(top, text="Distribution", padding=10)
    right.pack(side="left", fill="x", expand=True, padx=(6, 0))
    right.columnconfigure(1, weight=1)
    make_labeled_entry(right, 0, "Dist dir", dist_dir_var, width=46, browse=True)
    make_labeled_entry(right, 1, "GitHub repo", github_repo_var)
    make_labeled_entry(right, 2, "Update branch", update_branch_var)
    make_labeled_entry(right, 3, "Update channel", update_channel_var)
    ttk.Label(right, text="Package mode").grid(row=4, column=0, sticky="w", padx=(0, 8), pady=3)
    package_mode_combo = ttk.Combobox(right, textvariable=package_mode_var, values=("filtered", "complete"), state="readonly")
    package_mode_combo.grid(row=4, column=1, columnspan=2, sticky="ew", pady=3)

    options = ttk.LabelFrame(outer, text="Options", padding=10)
    options.pack(fill="x", pady=(10, 0))
    checkboxes: list[tuple[str, tk.BooleanVar]] = [
        ("Client", build_client_var),
        ("Server", build_server_var),
        ("Launcher", build_launcher_var),
        ("Fresh configure", fresh_var),
        ("Reconfigure", reconfigure_var),
        ("Clean first", clean_first_var),
        ("Run tests", ctest_var),
        ("No package", no_package_var),
        ("No dist", no_dist_var),
        ("Include steam_appid.txt", include_steam_appid_var),
        ("Bootstrap runtime", bootstrap_runtime_var),
        ("Verbose package", verbose_package_var),
        ("No metrics", no_metrics_var),
    ]
    for index, (label, variable) in enumerate(checkboxes):
        ttk.Checkbutton(options, text=label, variable=variable, command=update_command_preview).grid(row=index // 4, column=index % 4, sticky="w", padx=(0, 18), pady=3)

    advanced = ttk.LabelFrame(outer, text="Advanced package inputs; separate multiple entries with semicolons", padding=10)
    advanced.pack(fill="x", pady=(10, 0))
    advanced.columnconfigure(1, weight=1)
    make_labeled_entry(advanced, 0, "Payload dirs", package_payload_dirs_var, width=80)
    make_labeled_entry(advanced, 1, "Runtime roots", package_runtime_roots_var, width=80)
    make_labeled_entry(advanced, 2, "Include globs", package_include_globs_var, width=80)
    make_labeled_entry(advanced, 3, "Extra exclude globs", package_exclude_globs_var, width=80)
    make_labeled_entry(advanced, 4, "CMake defines", define_var, width=80)

    command_frame = ttk.LabelFrame(outer, text="Command preview", padding=10)
    command_frame.pack(fill="x", pady=(10, 0))
    command_entry = ttk.Entry(command_frame, textvariable=command_preview_var)
    command_entry.pack(side="left", fill="x", expand=True)
    copy_command_button = ttk.Button(command_frame, text="Copy", command=copy_command)
    copy_command_button.pack(side="left", padx=(8, 0))

    output_frame = ttk.LabelFrame(outer, text="Output", padding=10)
    output_frame.pack(fill="both", expand=True, pady=(10, 0))
    output_box = scrolledtext.ScrolledText(output_frame, wrap="word", height=16, state="disabled")
    output_box.pack(fill="both", expand=True)

    bottom = ttk.Frame(outer)
    bottom.pack(fill="x", pady=(10, 0))
    ttk.Label(bottom, textvariable=status_var).pack(side="left")
    open_dist_button = ttk.Button(bottom, text="Open dist folder", command=open_dist_folder)
    open_dist_button.pack(side="right")
    stop_button = ttk.Button(bottom, text="Stop", command=stop_build, state="disabled")
    stop_button.pack(side="right", padx=(0, 8))
    start_button = ttk.Button(bottom, text="Build / Dist", command=start_build)
    start_button.pack(side="right", padx=(0, 8))

    for variable in (
        source_dir_var,
        configure_preset_var,
        build_preset_var,
        test_preset_var,
        config_var,
        target_var,
        package_mode_var,
        update_channel_var,
        update_branch_var,
        github_repo_var,
        dist_dir_var,
        package_payload_dirs_var,
        package_runtime_roots_var,
        package_include_globs_var,
        package_exclude_globs_var,
        define_var,
        run_label_var,
    ):
        variable.trace_add("write", update_command_preview)

    update_command_preview()
    root.after(80, poll_output)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(run_gui() if should_launch_gui(sys.argv) else run_cli())
