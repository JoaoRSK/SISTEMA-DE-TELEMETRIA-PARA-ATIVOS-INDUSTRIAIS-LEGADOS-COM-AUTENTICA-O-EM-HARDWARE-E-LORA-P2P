# Ensaios de hardening do ESP32

Materiais empregados na avaliação independente de recursos de proteção de firmware e interfaces do ESP32.

## Arquivos

| Diretório | Descrição |
| --- | --- |
| `Visualizar Funcionamento/main.c` | Aplicação ESP-IDF de diagnóstico via serial. |
| `Secure Boot/Roteiro_Secure_Boot_V2.txt` | Registro do procedimento e dos resultados do ensaio de verificação de firmware assinado. |
| `Flash Encryption/Roteiro_Flash_Encryption.txt` | Registro do procedimento e das observações da proteção de conteúdo em flash. |
| `eFuses/Roteiro_eFuses_Hardening.txt` | Relato da configuração e da leitura dos campos de eFuse do dispositivo de ensaio. |

## Escopo

Estes arquivos se referem a um ambiente experimental controlado e **não demonstram, por si só, que todos os mecanismos estejam ativados no firmware principal do protótipo**. Os relatórios devem ser interpretados junto aos resultados da seção de segurança do TCC.

**Atenção:** procedimentos envolvendo *Secure Boot*, *Flash Encryption* e eFuses podem alterar permanentemente a configuração do microcontrolador e torná-lo inutilizável para outros testes. Os roteiros documentam ensaios já realizados; não devem ser executados sem conhecimento técnico, equipamento de teste apropriado e avaliação de seus efeitos irreversíveis.

O projeto completo de compilação e as chaves de assinatura não acompanham este repositório. **Nunca publique arquivos de chave privada, chaves de Flash Encryption ou cópias desprotegidas de material criptográfico.**
