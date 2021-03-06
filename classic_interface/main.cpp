#include "classic_interface.h"
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

namespace classic_fr_drv_ng_util {
/**
 * @brief checkResult проверяет код ошибки и бросает исключение если таковая
 * @param code
 */
static void checkResult(int code)
{
    if (code != 0) {
        std::stringstream ss;
        ss << "error, bad return code: " << code;
        throw std::runtime_error(ss.str());
    }
}

/**
 * @brief executeAndHandleError выполнить функцию и переповторить, если принтер занят печатью
 * @param func
 */
static void executeAndHandleError(std::function<int()> func)
{
    for (;;) {
        auto ret = func();
        switch (ret) {
        case 0x50: //Переповтор при ошибке "идёт печать предыдущей команды"
            continue;
        default:
            checkResult(ret);
            return;
        }
    }
}

/**
 * @brief isCashcore получить параметр кассы, реализация - кассовое ядро или аппаратный принтер
 * @param ci объект ci для которого необходимо выполнить
 * @return 0
 */
static int isCashcore(classic_interface* ci)
{
    ci->Set_ModelParamNumber(classic_interface::DPE_f23_cashcore);
    checkResult(ci->ReadModelParamValue());
    return ci->Get_ModelParamValue();
}

/**
 * @brief The PasswordHolder class
 * держатель паролей, восстанавливает пред. пароль при выходе из области видимости
 */
class PasswordHolder {
public:
    enum PasswordType {
        PT_User, //пользователь/кассир
        PT_Admin, //администратор
        PT_SC //пароль цто
    };

private:
    classic_interface* m_ci;
    uint32_t m_oldPassword;
    PasswordType m_passwordType;
    static uint32_t oldPasswordByType(classic_interface* ci, PasswordType passwordType)
    {
        switch (passwordType) {
        case PasswordHolder::PT_Admin:
            return ci->Get_SysAdminPassword();
        case PasswordHolder::PT_SC:
            return ci->Get_SCPassword();
        default:
            return ci->Get_Password();
        }
    }

public:
    PasswordHolder(const PasswordHolder&) = delete;
    PasswordHolder(const PasswordHolder&&) = delete;
    PasswordHolder& operator=(const PasswordHolder&) = delete;
    PasswordHolder& operator=(const PasswordHolder&&) = delete;
    PasswordHolder(classic_interface* ci, uint32_t tempPasword, PasswordType passwordType = PT_User)
        : m_ci(ci)
        , m_oldPassword(oldPasswordByType(ci, passwordType))
        , m_passwordType(passwordType)
    {
        switch (m_passwordType) {
        case PasswordHolder::PT_User:
            m_ci->Set_Password(tempPasword);
            break;
        case PasswordHolder::PT_Admin:
            m_ci->Set_SysAdminPassword(tempPasword);
            break;
        case PasswordHolder::PT_SC:
            m_ci->Set_SCPassword(tempPasword);
            break;
        }
    }
    ~PasswordHolder()
    {
        switch (m_passwordType) {
        case PasswordHolder::PT_User:
            m_ci->Set_Password(m_oldPassword);
            break;
        case PasswordHolder::PT_Admin:
            m_ci->Set_SysAdminPassword(m_oldPassword);
            break;
        case PasswordHolder::PT_SC:
            m_ci->Set_SCPassword(m_oldPassword);
            break;
        }
    }
};

static void prepareReceipt(classic_interface* ci)
{
    executeAndHandleError(std::bind(&classic_interface::WaitForPrinting, ci));
    executeAndHandleError(std::bind(&classic_interface::GetECRStatus, ci)); //получаем статус
    switch (ci->Get_ECRMode()) {
    case classic_interface::PM_SessionOpenOver24h: {
        {
            PasswordHolder ph(ci, ci->Get_SysAdminPassword()); // для снятия Z отчета необходимо
                // воспользоваться паролем администратора
            executeAndHandleError(std::bind(&classic_interface::PrintReportWithCleaning,
                ci)); //снимаем Z отчет если смена больше 24 часов
            executeAndHandleError(std::bind(&classic_interface::WaitForPrinting,
                ci)); //ждём пока отчет распечатается и открываем смену
        }
        executeAndHandleError(std::bind(&classic_interface::OpenSession,
            ci)); //смена закрыта - открываем
    }; break;
    case classic_interface::PM_SessionClosed:
        executeAndHandleError(std::bind(&classic_interface::OpenSession,
            ci)); //если смена закрыта - открываем
        break;
    case classic_interface::PM_OpenedDocument:
        executeAndHandleError(std::bind(&classic_interface::SysAdminCancelCheck,
            ci)); // отменяем документ если открыт
        break;
    }
    executeAndHandleError(std::bind(&classic_interface::WaitForPrinting, ci));
}

static void exchangeBytes(classic_interface* ci)
{
    ci->Set_BinaryConversion(classic_interface::TBinaryConversion::BINARY_CONVERSION_HEX);
    ci->Set_TransferBytes("FC");
    executeAndHandleError(std::bind(&classic_interface::ExchangeBytes, ci));
}
} // namespace classic_fr_drv_ng_util

using classic_fr_drv_ng_util::checkResult;
using classic_fr_drv_ng_util::exchangeBytes;
using classic_fr_drv_ng_util::executeAndHandleError;
using classic_fr_drv_ng_util::isCashcore;
using classic_fr_drv_ng_util::PasswordHolder;
using classic_fr_drv_ng_util::prepareReceipt;

/**
 * @brief print2DBarcode печать QR кода
 * @param ci
 */
static void print2DBarcode(classic_interface* ci)
{
    ci->Set_BarCode(
        u8"это печать unicode строки в QR коде с выравниванием по разным сторонам чека");
    ci->Set_BlockNumber(0);
    ci->Set_BarcodeType(classic_interface::BC2D_QRCODE);
    ci->Set_BarcodeParameter1(0); //авто версия
    ci->Set_BarcodeParameter3(4); //размер точки
    ci->Set_BarcodeParameter5(3); //уровень коррекции ошибок 0...3=L,M,Q,H
    for (const auto& alignment : { classic_interface::TBarcodeAlignment::baCenter,
             classic_interface::TBarcodeAlignment::baLeft,
             classic_interface::TBarcodeAlignment::baRight }) {
        ci->Set_BarcodeAlignment(alignment);
        executeAndHandleError(std::bind(&classic_interface::LoadAndPrint2DBarcode, ci));
    }
}

/**
 * @brief print1Dbarcode одномерные штрих-коды
 * @param ci
 * @param codeData
 */
static void print1Dbarcode(classic_interface* ci, const std::string& codeData)
{
    ci->Set_BarCode(codeData);
    ci->Set_LineNumber(50); //высота ШК в линиях
    ci->Set_BarWidth(2); //ширина вертикальной линии ШК
    executeAndHandleError(std::bind(&classic_interface::WaitForPrinting, ci));

    for (const auto& codeType : { classic_interface::BC1D_Code128A, classic_interface::BC1D_Code39,
             classic_interface::BC1D_EAN13 }) {
        ci->Set_BarcodeType(codeType); //тип
        for (const auto& alignment : { classic_interface::TBarcodeAlignment::baCenter,
                 classic_interface::TBarcodeAlignment::baLeft,
                 classic_interface::TBarcodeAlignment::baRight }) {
            ci->Set_BarcodeAlignment(alignment); //выравнивание
            for (const auto& text_alignment :
                { classic_interface::BCT_None, classic_interface::BCT_Above,
                    classic_interface::BCT_Below, classic_interface::BCT_Both }) {
                ci->Set_PrintBarcodeText(text_alignment); //печать текста ШК
                executeAndHandleError(std::bind(&classic_interface::PrintBarcodeLine, ci));
                executeAndHandleError(std::bind(&classic_interface::WaitForPrinting, ci));
            }
        }
    }
}

/**
 * @brief cashierReceipt
 * @param ci
 * @param cancel отменить чек
 */
static void cashierReceipt(classic_interface* ci, bool cancel = false)
{
    prepareReceipt(ci);
    ci->Set_CheckType(0); //продажа
    executeAndHandleError(
        std::bind(&classic_interface::OpenCheck, ci)); //открываем чек с паролем кассира
    ci->Set_Quantity(1.0);
    ci->Set_Department(0);
    ci->Set_Price(10000);
    ci->Set_Tax1(0);
    ci->Set_Tax2(0);
    ci->Set_Tax3(0);
    ci->Set_Tax4(0);
    ci->Set_StringForPrinting(u8"Молоко");
    executeAndHandleError(std::bind(&classic_interface::Sale, ci));
    if (cancel) {
        executeAndHandleError(std::bind(&classic_interface::SysAdminCancelCheck, ci));
        return;
    }
    ci->Set_Summ1(100000);
    ci->Set_Summ2(0);
    ci->Set_Summ3(0);
    ci->Set_Summ4(0);
    ci->Set_Summ5(0);
    ci->Set_Summ6(0);
    ci->Set_Summ7(0);
    ci->Set_Summ8(0);
    ci->Set_Summ9(0);
    ci->Set_Summ10(0);
    ci->Set_Summ11(0);
    ci->Set_Summ12(0);
    ci->Set_Summ13(0);
    ci->Set_Summ14(0);
    ci->Set_Summ15(0);
    ci->Set_Summ16(0);
    ci->Set_StringForPrinting(u8"строчка");
    executeAndHandleError(std::bind(&classic_interface::CloseCheck, ci));
    executeAndHandleError(std::bind(&classic_interface::WaitForPrinting, ci));
}
/*!
 * \brief fsOperationReturnReceipt возврат прихода с применением FNOperation
 * \param ci
 */
static void fsOperationReturnReceipt(classic_interface* ci)
{
    struct Item {
        int64_t price;
        double quantity;
        const char* name;
    };
    const Item items[] = {
        { 10000, 1, u8"Хороший товар" },
        { 4440, 4, u8"Плохой товар" },
        { 4200, 3, u8"Злой товар" },
    };
    int64_t sum = 0;
    prepareReceipt(ci);
    ci->Set_CheckType(2); //возврат прихода
    executeAndHandleError(std::bind(&classic_interface::OpenCheck, ci));

    for (const auto& item : items) {
        ci->Set_CheckType(2); //возврат прихода
        ci->Set_Quantity(item.quantity);
        ci->Set_Price(item.price);
        sum += ci->Get_Quantity() * ci->Get_Price();
        ci->Set_Summ1Enabled(false); //рассчитывает касса
        ci->Set_TaxValueEnabled(false);
        ci->Set_Tax1(1); //НДС 18%
        ci->Set_Department(1);
        ci->Set_PaymentTypeSign(4); //полный рассчет
        ci->Set_PaymentItemSign(1); //товар
        ci->Set_StringForPrinting(item.name);
        executeAndHandleError(std::bind(&classic_interface::FNOperation, ci));
    }
    auto electro_sum = sum - 12345; //частичный возврат наличными, частичный электронными
    ci->Set_Summ1(sum - electro_sum); // Наличные
    ci->Set_Summ2(electro_sum); //Электронными
    ci->Set_Summ3(0);
    ci->Set_Summ4(0);
    ci->Set_Summ5(0);
    ci->Set_Summ6(0);
    ci->Set_Summ7(0);
    ci->Set_Summ8(0);
    ci->Set_Summ9(0);
    ci->Set_Summ10(0);
    ci->Set_Summ11(0);
    ci->Set_Summ12(0);
    ci->Set_Summ13(0);
    ci->Set_Summ14(0);
    ci->Set_Summ15(0);
    ci->Set_Summ16(0);
    ci->Set_RoundingSumm(99); // Сумма округления
    ci->Set_TaxValue1(0); // Налоги мы не считаем
    ci->Set_TaxValue2(0);
    ci->Set_TaxValue3(0);
    ci->Set_TaxValue4(0);
    ci->Set_TaxValue5(0);
    ci->Set_TaxValue6(0);
    ci->Set_TaxType(1); // Основная система налогообложения
    ci->Set_StringForPrinting("");
    executeAndHandleError(std::bind(&classic_interface::FNCloseCheckEx, ci));
}

using AssignMark = int(classic_interface*);

/**
 * Передача кода маркировки через тег 1162
 *
 * Из кода маркировки самостояльно формируется значение тега 1162 в соответствии с ФФД
 * и в ККТ передается уже готовы данные.
 *
 * Работает вне зависимости от версии драйвера и прошивки ККТ.
*/
int assignMarkWithTag1162(classic_interface* ci)
{
    ci->Set_TagNumber(1162);
    ci->Set_TagType(9); // ttByteArray
    ci->Set_BinaryConversion(classic_interface::TBinaryConversion::BINARY_CONVERSION_HEX);
    ci->Set_TagValueBin(
        "524652552D3430313330312D41414130323737303331"); // марка: RU-401301-AAA0277031
    return ci->FNSendTagOperation();
}

/**
 * Передача кода маркировка посредством метода FNSendItemCodeData
 *
 * Из кода маркировки самостояльно извлекаются MarkingType, GTIN, SerialNumber,
 * передаются в драйвер, драйвер из них формирует тег 1162 и передает в ККТ.
 *
 * Устарел.
 *
*/
int assignMarkWithFNSendItemCodeData(classic_interface* ci)
{
    // марка 00000046198488X?io+qCABm8wAYa
    ci->Set_MarkingType(0x444d);
    ci->Set_GTIN("00000046198488");
    ci->Set_SerialNumber("X?io+qCABm8");
    return ci->FNSendItemCodeData();
}

/**
 * Передача кода маркировка посредством метода FNSendItemBarcode
 *
 * В ККТ напрямую отправляется код маркировки.
 *
 * Требуется актуальная версия прошивки ККТ и функциональная лицензия.
 *
*/
int assignMarkWithFNSendItemBarcode(classic_interface* ci)
{
    // марка 010460406000600021N4N57RSCBUZTQ\x1d2403004002910161218\x1d1724010191ffd0\x1d92tIAF/YVoU4roQS3M/m4z78yFq0fc/WsSmLeX5QkF/YVWwy8IMYAeiQ91Xa2z/fFSJcOkb2N+uUUmfr4n0mOX0Q==
    ci->Set_BarCode("010460406000600021N4N57RSCBUZTQ\x1d"
                    "2403004002910161218\x1d"
                    "1724010191ffd0\x1d"
                    "92tIAF/YVoU4roQS3M/m4z78yFq0fc/WsSmLeX5QkF/YVWwy8IMYAeiQ91Xa2z/"
                    "fFSJcOkb2N+uUUmfr4n0mOX0Q==");
    return ci->FNSendItemBarcode();
}

/**
 * @brief fsOperationReceipt продвинутыми командами ФН
 * Передача КТН разными способами
 * @param ci
 */
static void fsOperationReceipt(classic_interface* ci)
{
    struct Item {
        int64_t price;
        double quantity;
        int64_t total; // -1 - рассчитывает касса, иначе сами
        const char* name;
        bool bGTINexample; // пример товара с маркировкой
        AssignMark* assignMethod;
    };
    const Item items[] = {
        { 12300, 1.009456, -1, u8"Традиционное молоко", false },
        { 4440, 4.0, 17761, u8"Товар", false }, //сумма с *ошибкой* на копейку
        { 99900, 1.0, -1, u8"Шуба", true, &assignMarkWithTag1162 },
        { 5000, 1.0, -1, u8"Сигареты Прима", true, &assignMarkWithFNSendItemCodeData },
        { 23990, 1, -1, u8"Ботинки мужские", true, &assignMarkWithFNSendItemBarcode },
    };

    prepareReceipt(ci);
    int64_t sum = 0;
    ci->Set_CheckType(0); //продажа
    executeAndHandleError(std::bind(&classic_interface::OpenCheck, ci));

    for (const auto& item : items) {
        ci->Set_CheckType(1); //приход
        ci->Set_Quantity(item.quantity);
        ci->Set_Price(item.price);
        sum += item.price * item.quantity;
        ci->Set_Summ1Enabled(!(item.total == -1)); //рассчитывает касса
        ci->Set_Summ1(item.total);
        ci->Set_TaxValueEnabled(false);
        ci->Set_Tax1(1); //НДС 18%
        ci->Set_Department(1);
        ci->Set_PaymentTypeSign(4); //полный рассчет
        ci->Set_PaymentItemSign(1); //товар
        ci->Set_StringForPrinting(item.name);
        if (item.bGTINexample) {
            auto cashcore = isCashcore(ci); // узнаем на кассовом ядре мы работаем или нет
            if (cashcore) {
                //посылать тег, привязанный к операции на cashcore(Кассовое Ядро) нужно ДО операции
                executeAndHandleError(std::bind(item.assignMethod, ci));
            }
            executeAndHandleError(std::bind(&classic_interface::FNOperation, ci));
            if (!cashcore) {
                //иначе ПОСЛЕ
                executeAndHandleError(std::bind(item.assignMethod, ci));
            }
        } else {
            executeAndHandleError(std::bind(&classic_interface::FNOperation, ci));
        }
    }
    auto cash_sum = 15000; //частично оплатим начлиными
    auto electro_sum = sum - cash_sum; //остальное электронными
    ci->Set_Summ1(cash_sum); // Наличные
    ci->Set_Summ2(electro_sum); //Электронными
    ci->Set_Summ3(0);
    ci->Set_Summ4(0);
    ci->Set_Summ5(0);
    ci->Set_Summ6(0);
    ci->Set_Summ7(0);
    ci->Set_Summ8(0);
    ci->Set_Summ9(0);
    ci->Set_Summ10(0);
    ci->Set_Summ11(0);
    ci->Set_Summ12(0);
    ci->Set_Summ13(0);
    ci->Set_Summ14(0);
    ci->Set_Summ15(0);
    ci->Set_Summ16(0);
    ci->Set_RoundingSumm(99); // Сумма округления
    ci->Set_TaxValue1(0); // Налоги мы не считаем
    ci->Set_TaxValue2(0);
    ci->Set_TaxValue3(0);
    ci->Set_TaxValue4(0);
    ci->Set_TaxValue5(0);
    ci->Set_TaxValue6(0);
    ci->Set_TaxType(1); // Основная система налогообложения
    ci->Set_StringForPrinting("");
    executeAndHandleError(std::bind(&classic_interface::FNCloseCheckEx, ci));
}

static void adminCancelReceipt(classic_interface* ci)
{
    cashierReceipt(ci, true);
}

/**
 * @brief writeServiceTable пример записи служебной таблицы с паролем ЦТО
 */
static void writeServiceTable(classic_interface* ci)
{
    prepareReceipt(ci);
    auto valueToWrite = 30;
    {
        ci->Set_TableNumber(10);
        ci->Set_RowNumber(1);
        ci->Set_FieldNumber(10);
        executeAndHandleError(std::bind(&classic_interface::ReadTable, ci));
        //получили, закешировали структуру поля из служебной таблицы. Можно поменять пароль
        //администратора и изменить содержимое через WriteTable
        ci->Set_ValueOfFieldInteger(valueToWrite);
        {
            PasswordHolder ph(ci, ci->Get_SCPassword(), PasswordHolder::PT_Admin);
            executeAndHandleError(std::bind(&classic_interface::WriteTable, ci));
        }
    }
}
static void printLineSwaps(classic_interface* ci)
{
    prepareReceipt(ci);
    std::string line { 1, 2, 4, 8, 16, 32, 64, static_cast<char>(128), 64, 32, 16, 8, 4, 2, 1 };
    ci->Set_LineData(line);
    ci->Set_LineNumber(50);
    for (const auto& swapMode : { classic_interface::SBM_Swap, classic_interface::SBM_NoSwap,
             classic_interface::SBM_Prop, classic_interface::SBM_Model }) {
        ci->Set_SwapBytesMode(swapMode);
        executeAndHandleError(std::bind(&classic_interface::PrintLine, ci));
        executeAndHandleError(std::bind(&classic_interface::WaitForPrinting, ci));
    }
}

static void printBarcodeLineSwaps(classic_interface* ci)
{
    prepareReceipt(ci);
    ci->Set_DelayedPrint(true);
    for (const auto& swapMode : { classic_interface::SBM_Swap, classic_interface::SBM_NoSwap,
             classic_interface::SBM_Prop, classic_interface::SBM_Model }) {
        {
            std::stringstream ss;
            ss << "mode: " << swapMode;
            ci->Set_BarCode(ss.str());
        }
        ci->Set_LineNumber(50);
        ci->Set_SwapBytesMode(swapMode);
        ci->Set_BarWidth(2);
        ci->Set_BarcodeType(classic_interface::BC1D_Code128A);
        ci->Set_BarcodeAlignment(classic_interface::TBarcodeAlignment::baCenter);
        ci->Set_PrintBarcodeText(classic_interface::BCT_Below);
        executeAndHandleError(std::bind(&classic_interface::PrintBarcodeLine, ci));
        executeAndHandleError(std::bind(&classic_interface::WaitForPrinting, ci));
    }
}

static void printBasicLines(classic_interface* ci)
{
    prepareReceipt(ci);

    ci->Set_UseReceiptRibbon(true); //чековая лента
    ci->Set_StringForPrinting("строчка");
    executeAndHandleError(std::bind(&classic_interface::PrintString, ci));
    ci->Set_StringForPrinting(u8"Мой дядя самых честных правил,\n"
                              "Когда не в шутку занемог,\n"
                              "Он уважать себя заставил\n"
                              "И лучше выдумать не мог.\n"
                              "Его пример другим наука;\n"
                              "Но, боже мой, какая скука\n"
                              "С больным сидеть и день и ночь,\n"
                              "Не отходя ни шагу прочь!\n"
                              "Какое низкое коварство\n"
                              "Полуживого забавлять,\n"
                              "Ему подушки поправлять,\n"
                              "Печально подносить лекарство,\n"
                              "Вздыхать и думать про себя:\n"
                              "Когда же черт возьмет тебя!\n");
    ci->Set_CarryStrings(true);
    executeAndHandleError(std::bind(&classic_interface::PrintString, ci));
    ci->Set_FontType(1);
    {
        PasswordHolder ph(ci, ci->Get_SysAdminPassword());
        executeAndHandleError(std::bind(&classic_interface::GetFontMetrics, ci));
    }
    for (auto i = 1; i <= ci->Get_FontCount(); i++) {
        ci->Set_FontType(i);
        executeAndHandleError(std::bind(&classic_interface::PrintStringWithFont, ci));
    }
}

#ifdef WIN32
#define timegm _mkgmtime
#endif

/*!
 * \brief fsCorrectionReceipt пример печати чека коррекции FNBuildCorrectionReceipt2
 * \param ci
 */
static void fsCorrectionReceipt(classic_interface* ci)
{
    PasswordHolder ph(ci, ci->Get_SysAdminPassword());

    //начало чека коррекции
    executeAndHandleError(std::bind(&classic_interface::FNBeginCorrectionReceipt, ci));

    //отправка тэгов

    //устанавливаем тег 1177 наименование основания для коррекции
    ci->Set_TagNumber(1177);
    ci->Set_TagType(7); //Тип "строка"
    ci->Set_TagValueStr("correction name");
    executeAndHandleError(std::bind(&classic_interface::FNSendTag, ci));

    //здесь устанавливаем тег 1178 дата документа основания для коррекции
    ci->Set_TagNumber(1178);
    ci->Set_TagType(6); //Тип "время"
    std::time_t unixtime;
    { // 1.01.2018 - 48 лет с 1.01.1970, в секундах
        std::tm date20180101 = {};
        date20180101.tm_year = 118;
        date20180101.tm_mday = 1;
        date20180101.tm_isdst = -1;
        unixtime = timegm(&date20180101);
    }
    ci->Set_TagValueDateTime(unixtime);
    executeAndHandleError(std::bind(&classic_interface::FNSendTag, ci));

    //устанавливаем тег 1179 номер документа основания для коррекции
    ci->Set_TagNumber(1179);
    ci->Set_TagType(7); //Тип "строка"
    ci->Set_TagValueStr("12345");
    executeAndHandleError(std::bind(&classic_interface::FNSendTag, ci));

    //печать чека коррекции
    ci->Set_Summ1(3000); //сумма коррекции
    ci->Set_Summ2(2000);
    ci->Set_Summ3(1000);
    ci->Set_Summ4(0);
    ci->Set_Summ5(0);
    ci->Set_Summ6(0);
    ci->Set_Summ7(0);
    ci->Set_Summ8(0);
    ci->Set_Summ9(0);
    ci->Set_Summ10(0);
    ci->Set_Summ11(0);
    ci->Set_Summ12(0);
    ci->Set_CorrectionType(0); //Тип коррекции "самостоятельно"
    ci->Set_CalculationSign(1); //признак расчета "коррекция прихода"
    ci->Set_TaxType(1); //схема налогообложения "основная"
    executeAndHandleError(std::bind(&classic_interface::FNBuildCorrectionReceipt2, ci));
}

/*!
 * \brief fsRegistrationReport пример печати отчета о регистрации FNBuildRegistrationReport
 * \param ci
 */
static void fsRegistrationReport(classic_interface* ci)
{
    PasswordHolder ph(ci, ci->Get_SysAdminPassword());
    //заполняем таблицы
    // получаем номер таблицы "Фискальный Накопитель"
    ci->Set_ModelParamNumber(classic_interface::DPE_FsTableNumber);
    executeAndHandleError(std::bind(&classic_interface::ReadModelParamValue, ci));
    auto fsTableNumber = ci->Get_ModelParamValue();

    //Наименование пользователя
    ci->Set_TableNumber(fsTableNumber);
    ci->Set_RowNumber(1);
    ci->Set_FieldNumber(7);
    ci->Set_ValueOfFieldString("new user");
    executeAndHandleError(std::bind(&classic_interface::WriteTable, ci));

    //Адрес расчетов
    ci->Set_TableNumber(fsTableNumber);
    ci->Set_RowNumber(1);
    ci->Set_FieldNumber(9);
    ci->Set_ValueOfFieldString("new address");
    executeAndHandleError(std::bind(&classic_interface::WriteTable, ci));

    //Место расчетов
    ci->Set_TableNumber(fsTableNumber);
    ci->Set_RowNumber(1);
    ci->Set_FieldNumber(14);
    ci->Set_ValueOfFieldString("new place");
    executeAndHandleError(std::bind(&classic_interface::WriteTable, ci));

    //Начать формировать отчет о регистрации ККТ
    ci->Set_ReportTypeInt(1); //Отчет о регистрации КТТ
    executeAndHandleError(std::bind(&classic_interface::FNBeginRegistrationReport, ci));

    //посылаем необходимые теги
    ci->Set_TagNumber(1117); //тэг "адрес электронной почты отправителя чека"
    ci->Set_TagType(7); //Тип "строка"
    ci->Set_TagValueStr("example@example.org");
    executeAndHandleError(std::bind(&classic_interface::FNSendTag, ci));

    //Сформировать отчет о регистрации ККТ
    ci->Set_INN("000000000000");
    ci->Set_KKTRegistrationNumber("0000000837030527");
    ci->Set_TaxType(32); //система налогообложения - ПСН
    ci->Set_WorkMode(1); //режим работы - шифрование
    executeAndHandleError(std::bind(&classic_interface::FNBuildRegistrationReport, ci));
}

/*!
 * \brief ReceiptCopy пример печати "копии" чека
 * \param ci
 */
static void ReceiptCopy(classic_interface* ci)
{
    PasswordHolder ph(ci, ci->Get_SysAdminPassword());
    //необходимо заполнить свойство DocumentNumber - номер документа, копия которого нужна

    //если нужно напечатать копию документа номер 3
    // ci->Set_DocumentNumber(3);

    //если нужно напечатать копию последнего документа
    executeAndHandleError(std::bind(&classic_interface::FNGetStatus, ci));
    // DocumentNumber теперь равен номеру последнего документа

    ci->Set_ShowTagNumber(false);
    executeAndHandleError(std::bind(&classic_interface::FNGetDocumentAsString, ci));

    //все данные о чеке находятся в одной длинной строке StringForPrinting
    std::string str = ci->Get_StringForPrinting();

    //Данные разделены знаком новой строки. Разбиваем на строчки и печатаем
    std::size_t already_print = 0;
    std::string str2;
    std::size_t eos;

    while ((eos = str.find('\n', already_print)) != std::string::npos) {
        str2 = str.substr(already_print, eos - already_print);
        already_print = eos + 1;

        ci->Set_StringForPrinting(str2);
        executeAndHandleError(std::bind(&classic_interface::PrintString, ci));
    }

    str2 = str.substr(already_print);
    if (!str2.empty()) {
        ci->Set_StringForPrinting(str2);
        executeAndHandleError(std::bind(&classic_interface::PrintString, ci));
    }
}

/**
 * @brief skipPrintNextDocs управление печатью последующих документов
 * @param ci
 * @param doSkip пропуск
 */
void skipPrintNextDocs(classic_interface* ci, bool doSkip)
{
    ci->Set_DeviceFunctionNumber(classic_interface::DFE_SkipAllPrinting);
    ci->Set_ValueOfFunctionInteger(doSkip);
    executeAndHandleError(std::bind(&classic_interface::SetDeviceFunction, ci));
}
/**
 * @brief skipOneDocument как пропустить печать документа
 * @param ci
 */
void skipOneDocument(classic_interface* ci)
{
    skipPrintNextDocs(ci, true);
    fsOperationReceipt(ci);
    skipPrintNextDocs(ci, false);
}

int main(int argc, char* argv[])
{
    try {
        //        classic_interface::setLogCallback([](const std::string& logmsg) { std::cerr <<
        //        logmsg; });
        classic_interface ci;
        //        ci.setPropertyChangedCallback([](const std::string& property) {
        //            std::cout << "property modified: " << property << std::endl;
        //        });
        ci.Set_SCPassword(0); //Пароль ЦТО, нужен для установки нового пароля ЦТО + можно позже
            //воспользоваться для записи служебных таблиц(им необходим пароль ЦТО)
        ci.Set_SysAdminPassword(30); //Пароль сист. администратора
        ci.Set_Password(1); //Пароль кассира(может совпадать с паролем администратора)
        //        ci.Set_AutoEoD(true); //Включаем обмен с ОФД средствами драйвера
        if (argc > 1) {
            //можно передать URI в качестве аргумента
            ci.Set_ConnectionURI(argv[1]);
        } else {
            ci.Set_ConnectionURI(
                "tcp://192.168.137.111:7778?timeout=3000&bytetimeout=1500&protocol=v1");
        }
        checkResult(ci.Connect()); //соединяемся
        ci.Set_WaitForPrintingDelay(20); //задержка ожидания окончания печати
        printLineSwaps(&ci);
        printBarcodeLineSwaps(&ci);
        printBasicLines(&ci);
        fsOperationReceipt(&ci);
        ReceiptCopy(&ci);
        skipOneDocument(&ci);
        fsOperationReturnReceipt(&ci);
        fsCorrectionReceipt(&ci);
        // fsRegistrationReport(&ci);
        //        writeServiceTable(&ci); // пример записи сервисной таблицы
        exchangeBytes(&ci); //посылка произвольных данных
        cashierReceipt(&ci); //чек от кассира 1
        adminCancelReceipt(&ci); //открываем чек от имени кассира 1, отмена от администратора
        ci.Set_Password(2);
        cashierReceipt(&ci); //чек от кассира 2
        print1Dbarcode(&ci, "123456789"); //пример одномерных ШК
        print2DBarcode(&ci); //пример QR кода
        ci.Set_StringQuantity(10); //кол-во строк промотки
        ci.Set_UseReceiptRibbon(true); //использовать чековую лента
        ci.Set_CutType(false); //полная отрезка
        executeAndHandleError(std::bind(&classic_interface::FeedDocument, &ci)); //промотка
        executeAndHandleError(std::bind(&classic_interface::CutCheck, &ci)); //отрезка
        std::cerr << "all good" << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    } catch (...) {
        std::cerr << "unknown error" << std::endl;
    }
    return EXIT_FAILURE;
}
