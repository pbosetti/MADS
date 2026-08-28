#  __  __    _    ____  ____  
# |  \/  |  / \  |  _ \/ ___| 
# | |\/| | / _ \ | | | \___ \ 
# | |  | |/ ___ \| |_| |___) |
# |_|  |_/_/   \_\____/|____/ 
#
# Linux Systemd service file for {{service_name}}, a {{name}} agent
# Notice that the settings file will be read from 
# {{ini_file}}
#
# Save this file to {{systemd_path}}/{{service_name}}.service
# Or run "sudo {{this_exe}} service {{args}}" 
# then run "sudo systemctl enable {{service_name}}.service"

[Unit]
Description={{service_name}}
After=network.target
StartLimitIntervalSec=0

[Service]
Type=simple
Restart=always
RestartSec=1
User=root
# systemd hands a unit that does not say otherwise its DefaultLimitNOFILE soft
# value, which is 1024 on most distributions. The broker holds two descriptors
# per connected agent (its publisher and its subscriber), so that default caps
# a fleet at roughly 495 agents -- and libzmq refuses everything past it almost
# silently. Can also be set from the settings file with [broker] max_open_files,
# but only up to the hard limit this line establishes.
LimitNOFILE=65536
ExecStart={{command}}

[Install]
WantedBy=multi-user.target