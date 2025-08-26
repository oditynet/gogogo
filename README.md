<div align="center">
<img src="https://github.com/oditynet/gogogo/blob/main/logo.png" title="example" width="200" />
  <h1> gogogo </h1>
</div>

<img alt="GitHub code size in bytes" src="https://img.shields.io/github/languages/code-size/oditynet/gogogo"></img>
<img alt="GitHub license" src="https://img.shields.io/github/license/oditynet/gogogo"></img>
<img alt="GitHub commit activity" src="https://img.shields.io/github/commit-activity/m/oditynet/gogogo"></img>
<img alt="GitHub Repo stars" src="https://img.shields.io/github/stars/oditynet/gogogo"></img>

Bнициализация системы GOGOGO поможет вам быстро запустить службы после загрузки ядра, смонтировать ваши ФС иинициализировать ваши устройства, Udev, DBus. На видео пока я забыл подгрузить USB устройства и в моей DE miayDE не работает мышка, но это не проблема.
Поддерживается 3 режима работы: 1-однопользовательский,2-сетевой,3-графический

<img src="https://github.com/oditynet/gogogo/blob/main/image.gif" title="example" width="800" />

```
gcc gogogo.c -o gogogo
sudo cp gogogo /sbin
sudo mkdir -p /etc/gogogo/rc1/ /etc/gogogo/rc2/ /etc/gogogo/rc3/
sudo echo "2" | sudo tee /etc/gogogo/initlevel
sudo cp rc.devices  /etc/gogogo/
```

Конфигурационные файлы (на примере miayDE)
```
echo -e "NAME=miayDE\nCMD=/usr/sbin/miayDE\nRESTART=on-failure\nDEPENDS=network" | sudo tee /etc/gogogo/rc3/miayDE.conf
```

Run:
in grub edit line for example
```
... init=/sbin/gogogo ....
```
and press ctrl + x


