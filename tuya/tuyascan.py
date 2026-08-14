import tinytuya

print("Starting Tuya scan...")

d = tinytuya.OutletDevice(
    dev_id='bf141f396ef5025866yiza',
    address='192.168.50.178',
    local_key='PLJpfT&Be0QN:X[=',
    version=3.1  # Or 3.1, 3.4 etc.
)

d.set_socketPersistent(True)
d.set_socketTimeout(10)

data = d.status()
print(data)