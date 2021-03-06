/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */
#define VIRTUAL_UART_MODULE_ENABLED

/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "openamp.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "math.h"
#include "openamp_log.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

// Max command size received in Bytes
#define COMMAND_MAX_SIZE             100

// Computed CRC for "star"
#define CRC_BUFFER_SIZE              4

// Sine wave buffer size
#define SINE_BUFFER_SIZE             256

// ADC buffer size
#define ADC_BUFFER_SIZE              64

// Encrypted buffer size
#define CRY_BUFFER_SIZE              ((uint32_t)64)

// Synchronization mechanism configuration
#define COPRO_SYNC_SHUTDOWN_CHANNEL  IPCC_CHANNEL_3

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

CRC_HandleTypeDef hcrc2;

CRYP_HandleTypeDef hcryp2;
__ALIGN_BEGIN static const uint32_t pKeyCRYP2[4] __ALIGN_END = {
                            0x00000000,0x00000000,0x00000000,0x00000000};
DMA_HandleTypeDef hdma_cryp2_out;
DMA_HandleTypeDef hdma_cryp2_in;

DAC_HandleTypeDef hdac1;

HASH_HandleTypeDef hhash2;
DMA_HandleTypeDef hdma_hash2_in;

IPCC_HandleTypeDef hipcc;

TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim7;

/* USER CODE BEGIN PV */
extern __IO uint32_t uwTick;

static uint16_t wave_table[SINE_BUFFER_SIZE];
uint16_t constSineWaveIdx = 0;
volatile uint8_t mADCstate = 0;
uint16_t mADCcounter;
uint16_t m_nb_samp;

uint16_t VirtUart0ChannelRxSize = 0;
uint8_t VirtUart0ChannelBuffRx[COMMAND_MAX_SIZE];
static struct rpmsg_virtio_device rvdev;
VIRT_UART_HandleTypeDef huart0;

__IO FlagStatus VirtUart0RxMsg = RESET;
__IO FlagStatus AdcConvFinished = RESET;

char mBuffTx[512];
__ALIGN_BEGIN __IO uint8_t uhADCxConvertedValue[ADC_BUFFER_SIZE] __ALIGN_END;
__ALIGN_BEGIN uint8_t aEncryptedText[CRY_BUFFER_SIZE] __ALIGN_END;
__ALIGN_BEGIN static uint8_t aSHA256Digest[32] __ALIGN_END;
__ALIGN_BEGIN static uint8_t atext[8] __ALIGN_END;

volatile uint8_t crypt_enabled=1;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_CRC2_Init(void);
static void MX_CRYP2_Init(void);
static void MX_DAC1_Init(void);
static void MX_HASH2_Init(void);
static void MX_IPCC_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM7_Init(void);
int MX_OPENAMP_Init(int RPMsgRole, rpmsg_ns_bind_cb ns_bind_cb);
/* USER CODE BEGIN PFP */
static void MX_GPIO_DeInit(void);
static void MX_DMA_DeInit(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  TIMER period elapsed Callback .
  * @retval none
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance==TIM7) {
        constSineWaveIdx++;
        HAL_DAC_SetValue(&hdac1, DAC_CHANNEL_1, DAC_ALIGN_12B_R, wave_table[constSineWaveIdx % 256]);
    }
}

/**
  * @brief  ADC conversion success Callback .
  * @retval none
  */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* AdcHandle)
{
    if (mADCcounter < ADC_BUFFER_SIZE) {
        uhADCxConvertedValue[mADCcounter] = (HAL_ADC_GetValue(&hadc1) & 0xFF) / 2;
        mADCcounter++;
    } else {
        m_nb_samp = ADC_BUFFER_SIZE;
        AdcConvFinished = SET;
    }
}

/**
  * @brief  ADC conversion error Callback .
  * @retval none
  */
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef* hadc)
{
    log_err("ADC conversion error: file %s, line %d\r\n", file, line);
}

/**
  * @brief  Virtual UART RX Callback .
  * @retval none
  */
void VIRT_UART0_RxCpltCallback(VIRT_UART_HandleTypeDef *huart)
{
    // take only msg length > 2
    if (huart->RxXferSize > 2) {
        // copy received msg in a buffer
        VirtUart0ChannelRxSize = huart->RxXferSize < COMMAND_MAX_SIZE ? huart->RxXferSize : COMMAND_MAX_SIZE - 1;
        memcpy(VirtUart0ChannelBuffRx, huart->pRxBuffPtr, VirtUart0ChannelRxSize);
        VirtUart0RxMsg = SET;
    }
}

/**
  * @brief  Check received command.
  * @retval > 0 if command contains "Star"
  */
uint8_t treatCommand() {
      // wait: "Star abcdefgh" with abcdefgh is a uint32_t in hexa char
      uint32_t crc = 0;
      char *pStr = strstr ((char*)VirtUart0ChannelBuffRx,"Star");
      if ((pStr != 0) && (VirtUart0ChannelRxSize >= 13)) {
          // check CRC now
          for (int i=0; i<8; i++) {
              if (!((*(pStr+i+5) >= '0' && *(pStr+i+5) <= '9') || (*(pStr+i+5) >= 'a' && *(pStr+i+5) <= 'f'))) {
                  return 0;
                  break;
              }
          }
          crc = (uint32_t)strtoll((char*)(pStr+5), NULL, 16);
          return crc;
      } else {
          return 0;
      }
}

/**
  * @brief  HW reset of used peripheral.
  * @retval none
  */
void resetPeriph() {
    __HAL_RCC_TIM7_FORCE_RESET();
    __HAL_RCC_TIM2_FORCE_RESET();
    __HAL_RCC_HASH2_FORCE_RESET();
    __HAL_RCC_DAC12_FORCE_RESET();
    __HAL_RCC_CRYP2_FORCE_RESET();
    __HAL_RCC_CRC2_FORCE_RESET();
    __HAL_RCC_ADC12_FORCE_RESET();
    __HAL_RCC_DMA2_FORCE_RESET();
    HAL_Delay(2);
    __HAL_RCC_TIM7_RELEASE_RESET();
    __HAL_RCC_TIM2_RELEASE_RESET();
    __HAL_RCC_HASH2_RELEASE_RESET();
    __HAL_RCC_DAC12_RELEASE_RESET();
    __HAL_RCC_CRYP2_RELEASE_RESET();
    __HAL_RCC_CRC2_RELEASE_RESET();
    __HAL_RCC_ADC12_RELEASE_RESET();
    __HAL_RCC_DMA2_RELEASE_RESET();
}

/**
  * @brief  CallBack function which will be called when firmware will be stopped by Android application.
  * @retval none
  */
void CoproSync_ShutdownCb(IPCC_HandleTypeDef * hipcc, uint32_t ChannelIndex, IPCC_CHANNELDirTypeDef ChannelDir)
{
  /* Deinit the peripherals */
  HAL_TIM_Base_MspDeInit(&htim7);
  HAL_TIM_Base_MspDeInit(&htim2);
  HAL_HASH_MspDeInit(&hhash2);
  HAL_DAC_MspDeInit(&hdac1);
  HAL_CRYP_MspDeInit(&hcryp2);
  HAL_CRC_MspDeInit(&hcrc2);
  HAL_ADC_MspDeInit(&hadc1);
  MX_GPIO_DeInit();
  MX_DMA_DeInit();
  // reset all used peripherals
  resetPeriph();

  /* When ready, notify the remote processor that we can be shut down */
  HAL_IPCC_NotifyCPU(hipcc, ChannelIndex, IPCC_CHANNEL_DIR_RX);
}

/**
  * @brief  Prepare a table of 64 sine wave values to be generated by DAC1.
  * @retval none
  */
void initSinusWave() {
    double ret;
    for (int i=0; i<SINE_BUFFER_SIZE; i++) {
          ret = sin(2 * M_PI * i / SINE_BUFFER_SIZE)  * (2047 - 200) + 2148;
          wave_table[i] = (uint16_t)ret;
      }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  int n;
  int crypt_tries;
  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  // reset all used peripherals, as we are not sure of previous usage
  resetPeriph();

  /*HW semaphore Clock enable*/
  __HAL_RCC_HSEM_CLK_ENABLE();

  /* USER CODE END Init */

  if(IS_ENGINEERING_BOOT_MODE())
  {
    /* Configure the system clock */
    SystemClock_Config();
  }

  /* IPCC initialisation */
   MX_IPCC_Init();
  /* OpenAmp initialisation ---------------------------------*/
  MX_OPENAMP_Init(RPMSG_REMOTE, NULL);

  /* USER CODE BEGIN SysInit */
  HAL_IPCC_ActivateNotification(&hipcc, COPRO_SYNC_SHUTDOWN_CHANNEL, IPCC_CHANNEL_DIR_RX,
              CoproSync_ShutdownCb);

  /*
   * Create Virtual UART device
   * defined by a rpmsg channel attached to the remote device
   */
  huart0.rvdev =  &rvdev;
  if (VIRT_UART_Init(&huart0) != VIRT_UART_OK) {
    Error_Handler();
  }

  log_info("Cortex M4 boot successful with STM32Cube FW version: v%ld.%ld.%ld \n",
                                            ((HAL_GetHalVersion() >> 24) & 0x000000FF),
                                            ((HAL_GetHalVersion() >> 16) & 0x000000FF),
                                            ((HAL_GetHalVersion() >> 8) & 0x000000FF));

  /*Need to register callback for message reception by channels*/
  if(VIRT_UART_RegisterCallback(&huart0, VIRT_UART_RXCPLT_CB_ID, VIRT_UART0_RxCpltCallback) != VIRT_UART_OK)
  {
      log_err("Fatal error in VIRT_UART_RegisterCallback \n");
      Error_Handler();
  }

  log_info("Virtual UART0 OpenAMP-rpmsg channel creation \n");

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_CRC2_Init();
  MX_CRYP2_Init();
  MX_DAC1_Init();
  MX_HASH2_Init();
  MX_TIM2_Init();
  MX_TIM7_Init();
  /* USER CODE BEGIN 2 */
  HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED);
  HAL_DAC_Start(&hdac1, DAC_CHANNEL_1);
  initSinusWave();
  HAL_TIM_Base_Start_IT(&htim7);


  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      OPENAMP_check_for_message();

      if (VirtUart0RxMsg) {
        VirtUart0RxMsg = RESET;
        if (treatCommand()) {
            /* Compute the CRC of "Star" */
            strcpy ((char*)atext, "Star");
            __IO uint32_t uwCRCValue = HAL_CRC_Calculate(&hcrc2, (uint32_t *)atext, CRC_BUFFER_SIZE);

            // Transmit calculated CRC to the CA7
            n = sprintf(mBuffTx, "%u CRC=%x\n", (unsigned int)uwTick, (unsigned int)uwCRCValue);
            VIRT_UART_Transmit(&huart0, (uint8_t*)mBuffTx, n);
            HAL_Delay(10);

            // Start ADC1 conversion 8bit
            mADCcounter = 0;
            HAL_ADC_Start_IT(&hadc1);
            HAL_TIM_Base_Start(&htim2);
        }
      }

      if (AdcConvFinished) {
          AdcConvFinished = RESET;
          HAL_TIM_Base_Stop(&htim2);       // stop ADC sampling timer

          // Transmit ADC read samples to the CA7
          n = 0;
          n += sprintf(mBuffTx+n, "%u ADC=", (unsigned int)uwTick);
          for (int i=0; i<ADC_BUFFER_SIZE; i++) {
              n += sprintf(mBuffTx+n, "%x ", uhADCxConvertedValue[i]);
          }
          n += sprintf(mBuffTx+n, "\n");
          HAL_Delay(10);
          VIRT_UART_Transmit(&huart0, (uint8_t*)mBuffTx, n);

          // Compute HASH of the ADC read samples
          MX_HASH2_Init();
          if (HAL_HASHEx_SHA256_Start_DMA(&hhash2, (uint8_t*)uhADCxConvertedValue, ADC_BUFFER_SIZE) != HAL_OK)
          {
            Error_Handler();
          }

          // Wait for DMA transfer to complete
          while (HAL_HASH_GetState(&hhash2) == HAL_HASH_STATE_BUSY);

          // Get the computed digest value
          if (HAL_HASHEx_SHA256_Finish(&hhash2, aSHA256Digest, 0xFF) != HAL_OK)
          {
            Error_Handler();
          }

          // Transmit computed HASH to the CA7
          n = 0;
          n += sprintf(mBuffTx+n, "%u HASH=", (unsigned int)uwTick);
          for (int i=0; i<32; i++) {  // result of SHA256 is 32 bytes
              n += sprintf(mBuffTx+n, "%x ", aSHA256Digest[i]);
          }
          n += sprintf(mBuffTx+n, "\n");
          HAL_Delay(10);
          VIRT_UART_Transmit(&huart0, (uint8_t*)mBuffTx, n);

          // Do not start encryption if crypto not supported on the chip
          if (crypt_enabled) {
              // Start the AES encryption in ECB chaining mode with DMA
              if(HAL_CRYP_Encrypt_DMA(&hcryp2, (uint32_t*)uhADCxConvertedValue, CRY_BUFFER_SIZE, (uint32_t*)aEncryptedText) != HAL_OK)
              {
                  // Processing Error
                  Error_Handler();
              }

              /*
                Before starting a new process, you need to check the current state of the peripheral;
                if it is busy you need to wait for the end of current transfer before starting a new one.
                For simplicity reasons, this example is just waiting till the end of the process,
                but application may perform other tasks while transfer operation is ongoing.
              */
              crypt_tries = 100;
              while ((HAL_CRYP_GetState(&hcryp2) != HAL_CRYP_STATE_READY) && (crypt_tries > 0))
              {
                  crypt_tries--;
                  HAL_Delay(10);
              }

              // Transmit computed encrypted values to the CA7
              n = 0;
              n += sprintf(mBuffTx+n, "%u ENCR=", (unsigned int)uwTick);
              if (crypt_tries > 0) {
                  for (int i=0; i<CRY_BUFFER_SIZE; i++) {
                      n += sprintf(mBuffTx+n, "%x ", aEncryptedText[i]);
                  }
              }
              else {
                  n += sprintf(mBuffTx+n, "NOCRYPTO ");
                  crypt_enabled = 0;
                  log_info("Crypto IP not available \n");
              }
              n += sprintf(mBuffTx+n, "\n");
              HAL_Delay(10);
              VIRT_UART_Transmit(&huart0, (uint8_t*)mBuffTx, n);
          }
      }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_MEDIUMHIGH);
  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSI
                              |RCC_OSCILLATORTYPE_HSE|RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS_DIG;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = 16;
  RCC_OscInitStruct.HSIDivValue = RCC_HSI_DIV1;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLL12SOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 3;
  RCC_OscInitStruct.PLL.PLLN = 81;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 1;
  RCC_OscInitStruct.PLL.PLLR = 1;
  RCC_OscInitStruct.PLL.PLLFRACV = 0x800;
  RCC_OscInitStruct.PLL.PLLMODE = RCC_PLL_FRACTIONAL;
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL2.PLLSource = RCC_PLL12SOURCE_HSE;
  RCC_OscInitStruct.PLL2.PLLM = 3;
  RCC_OscInitStruct.PLL2.PLLN = 66;
  RCC_OscInitStruct.PLL2.PLLP = 2;
  RCC_OscInitStruct.PLL2.PLLQ = 1;
  RCC_OscInitStruct.PLL2.PLLR = 1;
  RCC_OscInitStruct.PLL2.PLLFRACV = 0x1400;
  RCC_OscInitStruct.PLL2.PLLMODE = RCC_PLL_FRACTIONAL;
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL3.PLLSource = RCC_PLL3SOURCE_HSE;
  RCC_OscInitStruct.PLL3.PLLM = 2;
  RCC_OscInitStruct.PLL3.PLLN = 34;
  RCC_OscInitStruct.PLL3.PLLP = 2;
  RCC_OscInitStruct.PLL3.PLLQ = 17;
  RCC_OscInitStruct.PLL3.PLLR = 37;
  RCC_OscInitStruct.PLL3.PLLRGE = RCC_PLL3IFRANGE_1;
  RCC_OscInitStruct.PLL3.PLLFRACV = 6660;
  RCC_OscInitStruct.PLL3.PLLMODE = RCC_PLL_FRACTIONAL;
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL4.PLLSource = RCC_PLL4SOURCE_HSE;
  RCC_OscInitStruct.PLL4.PLLM = 4;
  RCC_OscInitStruct.PLL4.PLLN = 99;
  RCC_OscInitStruct.PLL4.PLLP = 6;
  RCC_OscInitStruct.PLL4.PLLQ = 8;
  RCC_OscInitStruct.PLL4.PLLR = 8;
  RCC_OscInitStruct.PLL4.PLLRGE = RCC_PLL4IFRANGE_0;
  RCC_OscInitStruct.PLL4.PLLFRACV = 0;
  RCC_OscInitStruct.PLL4.PLLMODE = RCC_PLL_INTEGER;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** RCC Clock Config
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_ACLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3|RCC_CLOCKTYPE_PCLK4
                              |RCC_CLOCKTYPE_PCLK5|RCC_CLOCKTYPE_MPU;
  RCC_ClkInitStruct.MPUInit.MPU_Clock = RCC_MPUSOURCE_PLL1;
  RCC_ClkInitStruct.MPUInit.MPU_Div = RCC_MPU_DIV2;
  RCC_ClkInitStruct.AXISSInit.AXI_Clock = RCC_AXISSOURCE_PLL2;
  RCC_ClkInitStruct.AXISSInit.AXI_Div = RCC_AXI_DIV1;
  RCC_ClkInitStruct.MCUInit.MCU_Clock = RCC_MCUSSOURCE_PLL3;
  RCC_ClkInitStruct.MCUInit.MCU_Div = RCC_MCU_DIV1;
  RCC_ClkInitStruct.APB4_Div = RCC_APB4_DIV2;
  RCC_ClkInitStruct.APB5_Div = RCC_APB5_DIV4;
  RCC_ClkInitStruct.APB1_Div = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2_Div = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB3_Div = RCC_APB3_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Set the HSE division factor for RTC clock
  */
  __HAL_RCC_RTC_HSEDIV(24);
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */
  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_8B;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T2_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_6;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief CRC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC2_Init(void)
{

  /* USER CODE BEGIN CRC2_Init 0 */

  /* USER CODE END CRC2_Init 0 */

  /* USER CODE BEGIN CRC2_Init 1 */

  /* USER CODE END CRC2_Init 1 */
  hcrc2.Instance = CRC2;
  hcrc2.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc2.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
  hcrc2.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_BYTE;
  hcrc2.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_ENABLE;
  hcrc2.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
  if (HAL_CRC_Init(&hcrc2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC2_Init 2 */

  /* USER CODE END CRC2_Init 2 */

}

/**
  * @brief CRYP2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRYP2_Init(void)
{

  /* USER CODE BEGIN CRYP2_Init 0 */
  if (crypt_enabled)
  {
  /* USER CODE END CRYP2_Init 0 */

  /* USER CODE BEGIN CRYP2_Init 1 */

  /* USER CODE END CRYP2_Init 1 */
  hcryp2.Instance = CRYP2;
  hcryp2.Init.DataType = CRYP_DATATYPE_8B;
  hcryp2.Init.KeySize = CRYP_KEYSIZE_128B;
  hcryp2.Init.pKey = (uint32_t *)pKeyCRYP2;
  hcryp2.Init.Algorithm = CRYP_AES_ECB;
  hcryp2.Init.DataWidthUnit = CRYP_DATAWIDTHUNIT_BYTE;
  if (HAL_CRYP_Init(&hcryp2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRYP2_Init 2 */
  }
  /* USER CODE END CRYP2_Init 2 */

}

/**
  * @brief DAC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_DAC1_Init(void)
{

  /* USER CODE BEGIN DAC1_Init 0 */

  /* USER CODE END DAC1_Init 0 */

  DAC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN DAC1_Init 1 */

  /* USER CODE END DAC1_Init 1 */
  /** DAC Initialization
  */
  hdac1.Instance = DAC1;
  if (HAL_DAC_Init(&hdac1) != HAL_OK)
  {
    Error_Handler();
  }
  /** DAC channel OUT1 config
  */
  sConfig.DAC_HighFrequency = DAC_HIGH_FREQUENCY_INTERFACE_MODE_AUTOMATIC;
  sConfig.DAC_SampleAndHold = DAC_SAMPLEANDHOLD_DISABLE;
  sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
  sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_ENABLE;
  sConfig.DAC_ConnectOnChipPeripheral = DAC_CHIPCONNECT_DISABLE;
  sConfig.DAC_UserTrimming = DAC_TRIMMING_FACTORY;
  if (HAL_DAC_ConfigChannel(&hdac1, &sConfig, DAC_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DAC1_Init 2 */

  /* USER CODE END DAC1_Init 2 */

}

/**
  * @brief HASH2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_HASH2_Init(void)
{

  /* USER CODE BEGIN HASH2_Init 0 */

  /* USER CODE END HASH2_Init 0 */

  /* USER CODE BEGIN HASH2_Init 1 */

  /* USER CODE END HASH2_Init 1 */
  hhash2.Init.DataType = HASH_DATATYPE_8B;
  if (HAL_HASH_Init(&hhash2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN HASH2_Init 2 */

  /* USER CODE END HASH2_Init 2 */

}

/**
  * @brief IPCC Initialization Function
  * @param None
  * @retval None
  */
static void MX_IPCC_Init(void)
{

  /* USER CODE BEGIN IPCC_Init 0 */

  /* USER CODE END IPCC_Init 0 */

  /* USER CODE BEGIN IPCC_Init 1 */

  /* USER CODE END IPCC_Init 1 */
  hipcc.Instance = IPCC;
  if (HAL_IPCC_Init(&hipcc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IPCC_Init 2 */

  /* USER CODE END IPCC_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 13599;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM7 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM7_Init(void)
{

  /* USER CODE BEGIN TIM7_Init 0 */

  /* USER CODE END TIM7_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM7_Init 1 */

  /* USER CODE END TIM7_Init 1 */
  htim7.Instance = TIM7;
  htim7.Init.Prescaler = 0;
  htim7.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim7.Init.Period = 3400;
  htim7.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim7) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim7, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM7_Init 2 */

  /* USER CODE END TIM7_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMAMUX_CLK_ENABLE();
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream0_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
  /* DMA2_Stream1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
  /* DMA2_Stream2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();

}

/* USER CODE BEGIN 4 */
static void MX_DMA_DeInit(void)
{
  __HAL_RCC_DMAMUX_CLK_DISABLE();
  __HAL_RCC_DMA2_CLK_DISABLE();

  HAL_NVIC_DisableIRQ(DMA2_Stream0_IRQn);
  HAL_NVIC_DisableIRQ(DMA2_Stream1_IRQn);
  HAL_NVIC_DisableIRQ(DMA2_Stream2_IRQn);

}

static void MX_GPIO_DeInit(void)
{
  /* GPIO Ports Clock Disable */
  __HAL_RCC_GPIOC_CLK_DISABLE();
  __HAL_RCC_GPIOH_CLK_DISABLE();
  __HAL_RCC_GPIOA_CLK_DISABLE();
  __HAL_RCC_GPIOF_CLK_DISABLE();
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  log_err("OOOps: file %s, line %d\r\n", file, line);
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
