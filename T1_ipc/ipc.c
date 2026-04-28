/*
 * ipc.c — Módulo de IPC: fork() + pipes
 *
 * INF01142 — Sistemas Operacionais — 2026/1
 *
 * =========================================================================
 * ESTE É O ÚNICO ARQUIVO QUE DEVE SER MODIFICADO.
 * =========================================================================
 *
 * Implemente todas as funções declaradas em ipc.h.
 * Consulte os comentários em ipc.h para o contrato de cada função.
 *
 * Chamadas de sistema que você precisará:
 *   pipe(2), fork(2), read(2), write(2), close(2),
 *   waitpid(2), select(2) ou poll(2), exit(3), malloc(3), free(3)
 *
 * Referências rápidas:
 *   man 2 pipe      man 2 fork     man 2 waitpid
 *   man 2 select    man 2 read     man 2 write
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>

#include "mandelbrot.h"
#include "ipc.h"

/* =========================================================================
 * Definição interna do Pool
 *
 * Sugestão de estrutura — pode-se alterar caso necessário.
 * ========================================================================= */
typedef struct {
    pid_t pid;      /* PID do filho, ou -1 se a entrada está livre */
    int   read_fd;  /* descritor de leitura do pipe de retorno      */
} PoolEntry;

struct Pool {
    int        max;     /* capacidade máxima do pool  */
    int        active;  /* número de filhos ativos    */
    PoolEntry *entries; /* array de entradas          */
};

/* =========================================================================
 * pool_create
 * ========================================================================= */
Pool *pool_create(int max_children)
{
    Pool *pool = malloc(sizeof(Pool));
    if (!pool) return NULL;

    pool->max     = max_children;
    pool->active  = 0;
    pool->entries = malloc(sizeof(PoolEntry) * max_children);
    if (!pool->entries) { free(pool); return NULL; }

    for (int i = 0; i < max_children; i++) {
        pool->entries[i].pid     = -1;
        pool->entries[i].read_fd = -1;
    }
    return pool;
}

/* =========================================================================
 * pool_destroy
 * ========================================================================= */
void pool_destroy(Pool *pool)
{
    if (!pool) return;
    for (int i = 0; i < pool->max; i++) {
        if (pool->entries[i].read_fd != -1)
            close(pool->entries[i].read_fd);
    }
    free(pool->entries);
    free(pool);
}

/* =========================================================================
 * pool_active
 * ========================================================================= */
int pool_active(const Pool *pool)
{
    return pool->active;
}

/* =========================================================================
 * launch_worker
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void launch_worker(Pool *pool, const RenderParams *params, const Tile *t)
{

    int fd[2];
    if (pipe(fd) == -1)
    {
        perror("pipe");
        return;
    }
    
    if (pool->active >= pool->max) {perror("Max Child"); return;}
    pid_t pid = fork();
    
    if(pid<0) // erro
	{ 
        perror("fork"); 
        close(fd[0]); 
        close(fd[1]); 
        return;
    }
    
    if (pid == 0) // filho
    {
        close(fd[0]); // fecha leitor
        worker_main(params, t, fd[1]); // da kill
    }

    close(fd[1]); // fecha escrita
    /* Encontra slot livre (pid == -1) em vez de usar pool->active como índice */
    for (int i = 0; i < pool->max; i++) {
        if (pool->entries[i].pid == -1) {
            pool->entries[i].pid     = pid;
            pool->entries[i].read_fd = fd[0];
            pool->active++;
            break;
        }
    }
    
}

/* =========================================================================
 * worker_main
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void worker_main(const RenderParams *params, const Tile *tile, int write_fd)
{
    // Dica de estrutura:
    int n_pixels = tile->w * tile->h;
    unsigned char *buf = malloc(n_pixels);
    if (!buf) 
    { 
        perror("malloc");
        exit(1);
    }
    
    // Escrever cabeçalho: ox, oy, w, h
    write(write_fd, &(tile->ox), sizeof(int));
    write(write_fd, &(tile->oy), sizeof(int));
    write(write_fd, &(tile->w), sizeof(int));
    write(write_fd, &(tile->h), sizeof(int));

    // Lembre: use um loop para garantir que todos os bytes foram escritos!
    // Escrever pixels
    compute_tile(params, tile, buf);
    {
        ssize_t total = 0;
        while (total < n_pixels) {
            ssize_t ret = write(write_fd, buf + total, n_pixels - total);
            if (ret < 0) { perror("write pixels"); break; }
            total += ret;
        }
    }
    
    close(write_fd);
    free(buf);
    exit(0);
    
}

/* =========================================================================
 * pool_collect_ready
 *
 * TODO: implemente esta função.
 * ========================================================================= */
int pool_collect_ready(Pool *pool, TileResult *result)
{
    if (pool->active == 0) return 0;

    fd_set rfds;
    FD_ZERO(&rfds);
    int maxfd = -1;
    for (int i = 0; i < pool->max; i++) {
        if (pool->entries[i].read_fd != -1) {
            FD_SET(pool->entries[i].read_fd, &rfds);
            if (pool->entries[i].read_fd > maxfd) maxfd = pool->entries[i].read_fd;
        }
    }

    struct timeval tv = {0, 0}; 
    if (select(maxfd + 1, &rfds, NULL, NULL, &tv) <= 0) return 0;

    for (int i = 0; i < pool->max; i++) {
        int fd = pool->entries[i].read_fd;
        if (fd != -1 && FD_ISSET(fd, &rfds)) {
            if (read(fd, &result->tile, sizeof(Tile)) < (ssize_t)sizeof(Tile)) {
                return 0;
            }

            int n_pixels = result->tile.w * result->tile.h;
            result->pixels = malloc(n_pixels);

            ssize_t total = 0;
            while (total < n_pixels) {
                ssize_t ret = read(fd, result->pixels + total, n_pixels - total);
                if (ret <= 0) break;
                total += ret;
            }

            close(pool->entries[i].read_fd);
            pool->entries[i].read_fd = -1; 
            
            return 1;
        }
    }
    return 0;
}

/* =========================================================================
 * pool_reap
 *
 * TODO: implemente esta função.
 * ========================================================================= */
void pool_reap(Pool *pool)
{
    /* Dica de estrutura:
     *
     */
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < pool->max; i++) {
            if (pool->entries[i].pid == pid) {
                pool->entries[i].pid = -1;
                pool->active--;
            }
        }
    }
}
    
