import getpass
import paramiko

client = paramiko.SSHClient()
client.load_system_host_keys()
client.set_missing_host_key_policy(paramiko.WarningPolicy())
password = getpass.getpass('SSH password: ')
client.connect('192.168.2.238', username='ruilian', password=password,
               timeout=15, auth_timeout=15, look_for_keys=False, allow_agent=False)
command = "ls -la /opt; systemctl list-units --type=service --all --no-pager | grep -Ei 'hsf|emaster|gateway'; pgrep -af 'hsf_gateway|emaster'; find /home/ruilian /opt -maxdepth 4 -type f -name hsf_gateway 2>/dev/null; df -h /; ss -ltn | head -20"
_, output, errors = client.exec_command(command, timeout=20)
print(output.read().decode())
print(errors.read().decode())
client.close()
