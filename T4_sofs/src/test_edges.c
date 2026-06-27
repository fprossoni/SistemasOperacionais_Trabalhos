#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sofs.h"

#define ASSERT(expr, msg) \
    do { \
        if (!(expr)) { \
            printf("  [FALHA] %s\n", msg); \
            failures++; \
        } else { \
            printf("  [OK] %s\n", msg); \
            successes++; \
        } \
    } while (0)

int main(void) {
    int successes = 0;
    int failures = 0;
    int part = 0;
    int spb = 2;

    printf("=== INICIANDO CASOS DE TESTE EDGE ===\n\n");

    // Pre-test cleanup and format
    sofs_format(part, spb);

    // TEST 1: Limite de Montagem e Formatação
    printf("Teste 1: Limite de Montagem e Formatação\n");
    ASSERT(sofs_mount(part) == 0, "Montagem inicial deve funcionar");
    ASSERT(sofs_mount(part) == -1, "Montar partição já montada deve falhar");
    ASSERT(sofs_umount() == 0, "Desmontar deve funcionar");
    ASSERT(sofs_umount() == -1, "Desmontar partição que não está montada deve falhar");
    
    // Remount for subsequent tests
    sofs_mount(part);

    // TEST 2: Limite de arquivos abertos simultaneamente (máximo 10)
    printf("Teste 2: Limite de arquivos abertos simultaneamente\n");
    SOFS_FILE handles[11];
    char name_buf[32];
    int count = 0;
    for (int i = 0; i < 11; i++) {
        sprintf(name_buf, "file_%d.txt", i);
        handles[i] = sofs_create(name_buf);
        if (handles[i] >= 0) {
            count++;
        }
    }
    ASSERT(count == 10, "Deve conseguir abrir exatamente 10 arquivos");
    ASSERT(handles[10] < 0, "O 11º arquivo não deve ser aberto");
    
    // Close them
    for (int i = 0; i < 10; i++) {
        sofs_close(handles[i]);
    }

    // TEST 3: Handles Inválidos
    printf("Teste 3: Operações com Handles Inválidos ou Fechados\n");
    char dummy_buf[10];
    ASSERT(sofs_read(-1, dummy_buf, 5) < 0, "Ler de handle -1 deve falhar");
    ASSERT(sofs_read(10, dummy_buf, 5) < 0, "Ler de handle 10 deve falhar");
    ASSERT(sofs_write(5, dummy_buf, 5) < 0, "Escrever em handle não aberto/fechado deve falhar");
    ASSERT(sofs_close(5) < 0, "Fechar handle não aberto deve falhar");

    // TEST 4: Validação de Nomes de Arquivos
    printf("Teste 4: Validação de Nomes de Arquivos\n");
    char long_name[60];
    memset(long_name, 'a', 55);
    long_name[55] = '\0';
    ASSERT(sofs_create(long_name) < 0, "Criar arquivo com nome > 50 caracteres deve falhar");
    ASSERT(sofs_create("") < 0, "Criar arquivo com nome vazio deve falhar");

    // TEST 5: Truncamento de Arquivo Existente
    printf("Teste 5: Truncamento de Arquivo Existente\n");
    SOFS_FILE arq = sofs_create("trunc.txt");
    ASSERT(arq >= 0, "Criar trunc.txt deve funcionar");
    char dados[] = "Conteudo longo para preencher o arquivo";
    sofs_write(arq, dados, strlen(dados));
    sofs_close(arq);

    arq = sofs_create("trunc.txt"); // Recreate should truncate
    ASSERT(arq >= 0, "Recriar trunc.txt deve funcionar");
    char read_buf[64];
    memset(read_buf, 0, sizeof(read_buf));
    int bytes_lidos = sofs_read(arq, read_buf, 10);
    ASSERT(bytes_lidos == 0, "Arquivo truncado deve ter tamanho 0 (EOF imediato)");
    sofs_close(arq);

    // TEST 6: Detecção de Ciclo em Softlinks
    printf("Teste 6: Detecção de Ciclo em Softlinks\n");
    // Link A -> B e Link B -> A
    ASSERT(sofs_sln("link_A", "link_B") == 0, "Criar link_A -> link_B");
    ASSERT(sofs_sln("link_B", "link_A") == 0, "Criar link_B -> link_A");
    ASSERT(sofs_open("link_A") < 0, "Abrir link em loop cíclico deve falhar");

    // TEST 7: Ciclo de Vida do Hardlink e RefCounter
    printf("Teste 7: Ciclo de Vida do Hardlink e RefCounter\n");
    SOFS_FILE original = sofs_create("orig.txt");
    char texto[] = "LinkData";
    sofs_write(original, texto, strlen(texto));
    sofs_close(original);

    ASSERT(sofs_hln("orig_hard", "orig.txt") == 0, "Criar hardlink orig_hard -> orig.txt");
    ASSERT(sofs_delete("orig.txt") == 0, "Deletar arquivo original orig.txt");

    SOFS_FILE hl_fd = sofs_open("orig_hard");
    ASSERT(hl_fd >= 0, "Hardlink deve continuar acessível após deletar original");
    memset(read_buf, 0, sizeof(read_buf));
    sofs_read(hl_fd, read_buf, sizeof(read_buf) - 1);
    ASSERT(strcmp(read_buf, "LinkData") == 0, "Dados lidos do hardlink devem estar intactos");
    sofs_close(hl_fd);

    ASSERT(sofs_delete("orig_hard") == 0, "Deletar hardlink");

    // TEST 8: Operações de Diretório Inválidas
    printf("Teste 8: Operações de Diretório Inválidas\n");
    ASSERT(sofs_closedir() == -1, "Fechar diretório que não foi aberto deve falhar");
    ASSERT(sofs_opendir() == 0, "Abrir diretório raiz");
    SOFS_DIRENT dirent;
    ASSERT(sofs_readdir(&dirent) == 0, "Ler primeira entrada válida do diretório");
    sofs_closedir();
    ASSERT(sofs_readdir(&dirent) == -1, "Ler do diretório depois de fechar deve falhar");

    sofs_umount();

    printf("\n=== RESULTADO DOS TESTES ===\n");
    printf("Sucessos: %d / Falhas: %d\n", successes, failures);
    return failures == 0 ? 0 : 1;
}
