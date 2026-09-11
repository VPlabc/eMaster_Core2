import subprocess

startup = subprocess.STARTUPINFO()
startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
startup.wShowWindow = subprocess.SW_HIDE
subprocess.Popen([r'C:\Program Files\Docker\Docker\Docker Desktop.exe'], startupinfo=startup)
print('Docker Desktop launch requested.')
