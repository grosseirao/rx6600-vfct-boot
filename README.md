# RX6600 VFCT Boot

Aplicativo UEFI x64 para uma RX 6600M adaptada para desktop com o VBIOS RX 6600 `113-A49-X23`, em uma Erying Polestar G613 Pro com BIOS `THM570111`. Corrige o limite de 500 MHz observado com o driver AMD 32.0.21045.16010 ao reduzir o teto GFX de 1150 para 1050 mV na tabela ACPI VFCT, em memória, antes de iniciar o Windows.

## Resultado medido

Os dois ensaios usaram o mesmo driver, ajustes de fábrica pela ADLX e 25 segundos de OpenCL, com a SPPT removida e a tarefa de ajuste automático pausada.

| VFCT publicada | Máximo padrão pela ADLX | Teto GFX | Pico de clock medido |
| --- | ---: | ---: | ---: |
| Original | 500 MHz | 1150 mV | 500 MHz |
| Ajustada na partida | 2539 MHz | 1050 mV | 1766 MHz |

No ensaio ajustado, o pico entre as amostras com pelo menos 30% de utilização foi de 1632 MHz. O Windows manteve a identificação AMD Radeon RX 6600, PCI `1002:73FF`, e a ADLX continuou oferecendo ajuste de clock. O VBIOS da placa permaneceu intacto.

A mesma partida experimental permitiu alternar entre dois pacotes AMD assinados. Após instalar o 32.0.21033.3005, a GPU atingiu 1621 MHz. Ao atualizar de volta para o 32.0.21045.16010, atingiu 1768 MHz. Ambas as cargas OpenCL foram validadas com a SPPT removida, teto de 1050 mV, identificação RX 6600 e faixa de ajuste disponível.

O teste inicial de tensão usou uma versão que carregava o Windows a partir da mesma partição EFI. A versão deste projeto procura o carregador em volumes GPT NVMe, aceita o novo GUID de uma partição recriada e exige exatamente uma cópia de `\EFI\Microsoft\Boot\bootmgfw.efi`. A busca passou em simulação e na partida com o perfil completo. A reinstalação do Windows ainda precisa ser testada.

## Instalar a correção automática

O suporte verificado se limita à combinação de placa-mãe, VBIOS e captura descrita acima. O instalador copia o arquivo para a partição EFI do Windows. O registro é feito uma vez no UEFI Shell, com o Secure Boot desativado no equipamento do ensaio. A correção modifica a tabela ACPI em memória a cada partida.

1. Guarde em local privado a captura original e um perfil `.mpt` já testado no seu hardware. Se ainda precisar capturar a VFCT, faça isso no Windows em uma partida com a correção UEFI desativada. Use Python 3.10 ou mais recente, Clang, LLD e curl no PATH. O primeiro build baixa os cabeçalhos do EDK II de revisões fixadas.

   ```powershell
   python tools/build.py capture vfct-original.acpi
   $perfil = '.\perfil.mpt'
   $sha256 = (Get-FileHash -LiteralPath $perfil).Hash
   python tools/build.py build --vfct vfct-original.acpi --driver --mpt-profile $perfil --mpt-profile-sha256 $sha256
   ```

   O build gera `build/Rx6600DriverPerfil.efi`. Guarde o valor exibido em `VFCT SHA-256` para a verificação após reiniciar. A captura, o perfil e o binário compilado são arquivos locais e ficam fora dos arquivos de distribuição.

2. Na raiz do projeto, abra o PowerShell como administrador e copie o binário para a partição EFI.

   ```powershell
   .\tools\Copy-ProfileDriver.ps1
   ```

3. Reinicie para o **UEFI Shell**. Use `map -r` e localize a partição da cópia. O exemplo usa `fs0:`; ajuste o número de acordo com o resultado. Liste as entradas antes de adicionar para evitar duplicatas.

   ```text
   ls fs0:\EFI\RX6600-Test\Rx6600DriverPerfil.efi
   bcfg driver dump -v
   bcfg driver add 0 fs0:\EFI\RX6600-Test\Rx6600DriverPerfil.efi "RX6600 VFCT Perfil Completo"
   bcfg driver dump -v
   ```

   A nova entrada deve ter um caminho de hardware antes de `HD(...)`. Execute `reset` e deixe o **Windows Boot Manager** iniciar normalmente. [Especificação do UEFI Shell](https://uefi.org/sites/default/files/resources/UEFI_Shell_2_2.pdf#page=106)

4. Confirme a tabela recebida pelo Windows.

   ```powershell
   python tools/build.py capture vfct-ativa.acpi
   Get-FileHash .\vfct-ativa.acpi
   ```

   Compare o SHA-256 com o valor `VFCT SHA-256` exibido na compilação do seu perfil.

## Reinstalar ou remover

- **Após atualizar o driver AMD**, a entrada UEFI continua aplicando a correção na partida. A troca entre os dois pacotes AMD descritos abaixo foi testada.
- **Após reinstalar o Windows**, rode a verificação do hash. Se a partição EFI foi apagada, use o binário guardado ou recompile com as cópias privadas e repita a instalação a partir do passo 2. Caso a entrada antiga ainda exista e a partição tenha sido recriada, remova essa entrada antes de registrar o novo caminho.
- **Para remover a correção**, abra o UEFI Shell, execute `bcfg driver dump -v` e localize **RX6600 VFCT Perfil Completo**. Execute `bcfg driver rm N`, trocando `N` pela posição dessa entrada na lista. Reinicie pelo Windows Boot Manager e confira se o hash voltou ao da captura original. A remoção da entrada basta para desativar a correção.

## Compilar a variante de tensão

Use Python 3.10 ou mais recente, Clang, LLD e curl no PATH. A captura deve ser feita no Windows em uma partida normal com a VFCT original. O primeiro build baixa os cabeçalhos do EDK II de revisões fixadas.

```powershell
python tools/build.py capture vfct-original.acpi
python tools/build.py build --vfct vfct-original.acpi
```

O resultado fica em `build/Rx6600Boot.efi`. A captura deve ter SHA-256 `bf6276e5afa2860afb2df579fc70edcb32d9df15f62fd06a3c11c22126fb8d99`. Outra captura é recusada. O binário e o cabeçalho gerado contêm os dados de firmware capturados localmente. Mantenha ambos fora do Git e dos arquivos de release.

## Testar uma partida

1. Use um volume FAT32 separado, com o Secure Boot já desativado, como no hardware do ensaio. Copie o aplicativo para `\EFI\BOOT\BOOTX64.EFI` em uma mídia USB dedicada ou use **Boot from EFI File** para selecionar `Rx6600Boot.efi`.
2. Selecione o aplicativo pelo menu de boot para uma única partida. Mantenha a entrada **Windows Boot Manager** acessível. Caso precise voltar à VFCT original, reinicie e selecione essa entrada.
3. No Windows, capture novamente a VFCT para outro arquivo. O hash ajustado esperado é `e05382242cc07d6e1a7a598f2c2d9e0c4678e6fdea97f79092c91be07c540d91`. Ao medir o clock, desative temporariamente outros ajustes de GPU e restaure-os após a coleta.

Em um volume separado, o aplicativo pode permanecer disponível quando o disco do Windows for reparticionado. A instalação do Windows pode promover a entrada **Windows Boot Manager** na ordem de boot. A disponibilidade do arquivo EFI e a prioridade de inicialização são verificações separadas. [UEFI Boot Manager](https://uefi.org/specs/UEFI/2.11/03_Boot_Manager.html) · [BCDBoot](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdboot-command-line-options-techref-di)

## Variante experimental para carregamento automático

```powershell
python tools/build.py build --vfct vfct-original.acpi --driver
```

Esse comando gera `build/Rx6600Driver.efi`, um driver UEFI que pode ser carregado pelo firmware antes do Windows Boot Manager. Ele procura a VFCT ao ser carregado e novamente no evento `ReadyToBoot`, caso o firmware publique a tabela mais tarde. A tabela ajustada já presente passa sem nova gravação em memória.

Para um ensaio manual em uma única partida, use `load -nc fsN:\caminho\Rx6600Driver.efi` no UEFI Shell e, na mesma sessão, execute o `bootmgfw.efi` original da partição EFI do Windows. Confirme o hash ajustado no Windows.

Com um perfil completo, o driver `Rx6600DriverPerfil.efi` foi carregado automaticamente em hardware pela entrada persistente em `DriverOrder` com caminho completo. O arquivo estava na partição EFI do Windows. Com a SPPT removida, a GPU usou os limites do perfil no estado de fábrica do driver AMD, ultrapassou 500 MHz sob carga e completou o teste OpenCL de 25 segundos. O teste com caminho abreviado deixou a entrada registrada, mas o Windows recebeu a VFCT original. [UEFI Boot Manager](https://uefi.org/specs/UEFI/2.11/03_Boot_Manager.html)

Em uma partida posterior, o Windows recebeu a VFCT original mesmo com a entrada de caminho completo ainda registrada. A causa segue em investigação. Confira o hash da tabela após reiniciar para saber qual configuração foi aplicada.


## Funcionamento e testes

O aplicativo localiza o protocolo ACPI Support da AMI e confere a VFCT original byte a byte. Em seguida, carrega o Windows Boot Manager, substitui a VFCT e confere a tabela publicada antes de iniciar o carregador. Se o carregador retornar ou a verificação da troca falhar, tenta restaurar a tabela original. Uma nova partida reconstrói as tabelas ACPI.

Por padrão o build altera dois bytes de `MaxVoltageGfx` em unidades Q2 e recalcula o checksum legado. Com `--mpt-profile` e o SHA-256 do arquivo local, o build incorpora a tabela PowerPlay completa e gera `build/Rx6600BootPerfil.efi`. Com `--driver`, a variante com perfil gera `build/Rx6600DriverPerfil.efi`. O hash da VFCT gerada aparece na saída da compilação.

Na partida de teste com `Rx6600BootPerfil.efi`, o Windows recebeu a VFCT esperada. Com a SPPT removida, o perfil permaneceu ativo durante a troca para 32.0.21033.3005 e a atualização de volta para 32.0.21045.16010. Os ajustes feitos pelo XML do Adrenalin continuam sendo gerenciados pelo Windows.

```sh
python tools/build.py test
```

Os testes usam GCC e uma tabela sintética. Cobrem troca e restauração, falha de instalação, verificação divergente, VFCT duplicada, captura incompatível, novo GUID de partição, carregador ausente, carregadores ambíguos e exclusão de mídia USB como destino do Windows. Também verificam a publicação tardia da VFCT, a repetição de `ReadyToBoot` e a restauração pelo driver UEFI após uma falha de instalação. O código do projeto usa MIT. Os cabeçalhos baixados preservam seus avisos e licenças originais. [EDK II](https://github.com/tianocore/edk2)
