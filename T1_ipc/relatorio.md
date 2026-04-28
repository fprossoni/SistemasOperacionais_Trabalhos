## Relatório trabalho IPC

Alunos: 
- Felipe Pasinato Rossoni 587631
- Marcelo Gonda Stangler 587562

### Como pai evita bloquear
Usamos o `select()` para coletar a lista de filhos prontos, procurar os filhos prontos no array e ler o resultado, fechando o filho com close. Isso é realizado na função `pool_collect_ready`.
Usamos o `WNOHANG` para verificar quando o processo está finalizado, sem bloquear o processo pai. Depois, ele remove os processos filhos finalizados da pool de processos ativos.

### Estratégias de fechamento de FD
A estratégia de fechamento consiste em fechar o fd logo após o processo de leitura, e não na remoção do vetor de filhos. Portanto, isso acontece na função `pool_collect_ready`, e não na `pool_reap`. 

As consequencias de não fechar os descritores é que os processos filhos que são criados posteriormente não conseguem escrever no pipe. Assim, a imagem do fractal fica pixelada com pontos pretos, não completando o desenho, como pode ser visto na imagem abaixo:

![alt text](image.png)

### Declaração de uso de IA:
Um chat de IA foi usado para debugar um problema na função `pool_reap`, onde haviamos colocado o close do file descriptor de forma errada e a IA orientou a passar as duas linhas de fechamento do fd para a função `pool_collect_ready`. As linhas são:
```cpp
close(pool->entries[i].read_fd);
pool->entries[i].read_fd = -1;
```