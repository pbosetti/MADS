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
ExecStart={{command}}

[Install]
WantedBy=multi-user.target