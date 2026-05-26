#ifndef READ_LCD_ID_H
#define READ_LCD_ID_H

void lcd_gpio_init(void);
void SPI_ReadComm(uint8_t regval);
uint8_t SPI_ReadData(void);
uint8_t read_lcd_id(void);
#endif
